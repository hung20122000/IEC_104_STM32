#include "iec104/iec104_handlers.h"

#include "iec104/control_service.h"
#include "iec104/iec104_log.h"
#include "iec104/point_database.h"

#include "cs101_information_objects.h"
#include "hal_time.h"
#include "stm32h7xx_hal.h"

#include <string.h>

/* This imported lib60870 build exposes connection operations in its vtable;
 * the public IMasterConnection_* declarations have no linked wrapper body. */
#define IMasterConnection_sendASDU(c, a)      ((c)->sendASDU((c), (a)))
#define IMasterConnection_sendACT_CON(c, a, n) ((c)->sendACT_CON((c), (a), (n)))
#define IMasterConnection_sendACT_TERM(c, a)  ((c)->sendACT_TERM((c), (a)))

#define IEC104_PENDING_COMMANDS 8U

typedef struct
{
  bool occupied;
  uint32_t transaction_id;
  uint32_t session_id;
  IMasterConnection connection;
  bool result_ready;
  bool result_success;
  sCS101_StaticASDU request;
} PendingCommand;

typedef struct
{
  bool active;
  uint32_t session_id;
  size_t next_index;
  size_t count;
  IMasterConnection connection;
  sCS101_StaticASDU request;
  PointSnapshot points[IEC104_POINT_COUNT];
} PendingInterrogation;

static CS104_Slave slave_instance;
static IMasterConnection active_connection;
static uint32_t session_id;
static uint32_t next_transaction_id;
static uint32_t synchronized_at_tick;
static bool clock_synchronized;
static PendingCommand commands[IEC104_PENDING_COMMANDS];
static PendingInterrogation interrogation;
static bool spontaneous_pending;
static PointSnapshot spontaneous_event;

static bool SendPoint(IMasterConnection connection, const PointSnapshot *point,
                      CS101_CauseOfTransmission cause, bool with_time)
{
  sCS101_StaticASDU buffer;
  struct sCP56Time2a timestamp;
  InformationObject object = NULL;
  CS101_ASDU asdu;
  uint64_t event_time = point->source_utc_ms;
  bool sent = false;

  if (with_time && (event_time == 0U) && clock_synchronized &&
      ((int32_t)(point->sample_tick_ms - synchronized_at_tick) >= 0))
  {
    event_time = Hal_getTimeInMs() -
      (uint32_t)(HAL_GetTick() - point->sample_tick_ms);
  }

  with_time = with_time && (event_time != 0U);
  if (with_time)
    CP56Time2a_setFromMsTimestamp(&timestamp, event_time);

  asdu = CS101_ASDU_initializeStatic(&buffer,
                                      CS104_Slave_getAppLayerParameters(slave_instance),
                                      false, cause, 0, IEC104_COMMON_ADDRESS,
                                      false, false);
  if (asdu == NULL)
    return false;

  switch (point->type)
  {
    case POINT_TYPE_SINGLE:

      object = with_time ?
        (InformationObject)SinglePointWithCP56Time2a_create(NULL, point->ioa,
          point->value.single, point->quality, &timestamp) :
        (InformationObject)SinglePointInformation_create(NULL, point->ioa,
          point->value.single, point->quality);
      break;

    case POINT_TYPE_DOUBLE:

      object = with_time ?
        (InformationObject)DoublePointWithCP56Time2a_create(NULL, point->ioa,
          point->value.double_state, point->quality, &timestamp) :
        (InformationObject)DoublePointInformation_create(NULL, point->ioa,
          point->value.double_state, point->quality);
      break;

    case POINT_TYPE_STEP:

      object = with_time ?
        (InformationObject)StepPositionWithCP56Time2a_create(NULL, point->ioa,
          point->value.step.position, point->value.step.transient,
          point->quality, &timestamp) :
        (InformationObject)StepPositionInformation_create(NULL, point->ioa,
          point->value.step.position, point->value.step.transient,
          point->quality);
      break;

    case POINT_TYPE_NORMALIZED:

      object = with_time ?
        (InformationObject)MeasuredValueNormalizedWithCP56Time2a_create(NULL,
          point->ioa, point->value.normalized, point->quality, &timestamp) :
        (InformationObject)MeasuredValueNormalized_create(NULL, point->ioa,
          point->value.normalized, point->quality);
      break;

    case POINT_TYPE_SCALED:

      object = with_time ?
        (InformationObject)MeasuredValueScaledWithCP56Time2a_create(NULL,
          point->ioa, point->value.scaled, point->quality, &timestamp) :
        (InformationObject)MeasuredValueScaled_create(NULL, point->ioa,
          point->value.scaled, point->quality);
      break;

    case POINT_TYPE_FLOAT:

      object = with_time ?
        (InformationObject)MeasuredValueShortWithCP56Time2a_create(NULL,
          point->ioa, point->value.real, point->quality, &timestamp) :
        (InformationObject)MeasuredValueShort_create(NULL, point->ioa,
          point->value.real, point->quality);
      break;

    default:
      break;
  }

  if (object != NULL)
  {
    if (CS101_ASDU_addInformationObject(asdu, object))
    {
      sent = IMasterConnection_sendASDU(connection, asdu);
    }
    InformationObject_destroy(object);
  }

  return sent;
}

static bool IsActive(IMasterConnection connection)
{
  return (connection != NULL) && (connection == active_connection);
}

static bool AllowedCA(void *parameter, int ca)
{
  (void)parameter;
  return ca == IEC104_COMMON_ADDRESS;
}

static void ConnectionEvent(void *parameter, IMasterConnection connection,
                            CS104_PeerConnectionEvent event)
{
  (void)parameter;

  if (event == CS104_CON_EVENT_CONNECTION_OPENED)
  {
    ControlService_CancelSelection(session_id);
    session_id++;
    
    if (session_id == 0U)
      session_id++;

    active_connection = NULL;
    interrogation.active = false;
    spontaneous_pending = false;
    memset(commands, 0, sizeof(commands));

    IEC104_Log("IEC104: connection opened session=%lu\r\n",
               (unsigned long)session_id);
  }

  else if (event == CS104_CON_EVENT_ACTIVATED)
  {
    active_connection = connection;
    IEC104_Log("IEC104: master activated session=%lu\r\n",
               (unsigned long)session_id);
  }

  else if ((event == CS104_CON_EVENT_CONNECTION_CLOSED) ||
           (event == CS104_CON_EVENT_DEACTIVATED))
  {
    if (connection == active_connection)
      active_connection = NULL;

    ControlService_CancelSelection(session_id);

    interrogation.active = false;
    spontaneous_pending = false;

    memset(commands, 0, sizeof(commands));

    IEC104_Log("IEC104: master %s session=%lu\r\n",
               (event == CS104_CON_EVENT_CONNECTION_CLOSED) ?
                 "closed" : "deactivated", (unsigned long)session_id);
  }
}

static bool BeginInterrogation(IMasterConnection connection, CS101_ASDU asdu)
{
  size_t index;
  PointSnapshot point;

  if (!IsActive(connection) || interrogation.active ||
      (CS101_ASDU_getCOT(asdu) != CS101_COT_ACTIVATION))
  {
    (void)IMasterConnection_sendACT_CON(connection, asdu, true);
    IEC104_Log("IEC104: GI rejected (inactive, busy, or invalid COT)\r\n");

    return true;
  }

  interrogation.count = 0U;

  for (index = 0U; index < PointDatabase_Count(); index++)
  {
    if (PointDatabase_ReadByIndex(index, &point))
      interrogation.points[interrogation.count++] = point;
  }

  if (CS101_ASDU_clone(asdu, &interrogation.request) == NULL)
  {
    (void)IMasterConnection_sendACT_CON(connection, asdu, true);
    IEC104_Log("IEC104: GI rejected (request clone failed)\r\n");

    return true;
  }

  bool sent = IMasterConnection_sendACT_CON(connection, asdu, false);

  if (!sent)
    return true;

  interrogation.connection = connection;
  interrogation.session_id = session_id;
  interrogation.next_index = 0U;
  interrogation.active = true;

  IEC104_Log("IEC104: GI started points=%u\r\n",
             (unsigned)interrogation.count);

  return true;
}

static bool Interrogation(void *parameter, IMasterConnection connection,
                          CS101_ASDU asdu, uint8_t qoi)
{
  (void)parameter;

  if (qoi != IEC60870_QOI_STATION)
  {
    (void)IMasterConnection_sendACT_CON(connection, asdu, true);
    IEC104_Log("IEC104: GI rejected QOI=%u\r\n", (unsigned)qoi);

    return true;
  }
  return BeginInterrogation(connection, asdu);
}

static bool CounterInterrogation(void *parameter, IMasterConnection connection,
                                 CS101_ASDU asdu, QualifierOfCIC qcc)
{
  (void)parameter;
  (void)qcc;

  (void)IMasterConnection_sendACT_CON(connection, asdu, true);

  return true;
}

static bool ReadPoint(void *parameter, IMasterConnection connection,
                      CS101_ASDU asdu, int ioa)
{
  PointSnapshot point;
  (void)parameter;

  if (!IsActive(connection) || !PointDatabase_ReadByIOA((uint32_t)ioa, &point))
  {
    CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_IOA);
    CS101_ASDU_setNegative(asdu, true);

    (void)IMasterConnection_sendASDU(connection, asdu);

    return true;
  }

  (void)SendPoint(connection, &point, CS101_COT_REQUEST, false);

  return true;
}

static bool ClockSync(void *parameter, IMasterConnection connection,
                      CS101_ASDU asdu, CP56Time2a new_time)
{
  uint64_t utc_ms;
  struct sCP56Time2a local_time;
  (void)parameter;

  if (!IsActive(connection))
  {
    IEC104_Log("IEC104: ClockSync rejected (connection not active)\r\n");
    return false;
  }

  utc_ms = CP56Time2a_toMsTimestamp(new_time);
  if (utc_ms < 946684800000ULL)
  {
    IEC104_Log("IEC104: ClockSync rejected (time before year 2000)\r\n");
    return false;
  }

  /* UTC+7 is for display only; keep the internal clock in UTC. */
  CP56Time2a_setFromMsTimestamp(&local_time,
                              utc_ms + 7ULL * 60ULL * 60ULL * 1000ULL);

  IEC104_Log(
    "IEC104 RX ClockSync: CA=%u local(+07:00)=%04d-%02d-%02d %02d:%02d:%02d.%03d\r\n",
    (unsigned)CS101_ASDU_getCA(asdu),
    2000 + CP56Time2a_getYear(&local_time),
    CP56Time2a_getMonth(&local_time),
    CP56Time2a_getDayOfMonth(&local_time),
    CP56Time2a_getHour(&local_time),
    CP56Time2a_getMinute(&local_time),
    CP56Time2a_getSecond(&local_time),
    CP56Time2a_getMillisecond(&local_time));

  if (!Hal_setTimeInNs(utc_ms * 1000000ULL))
  {
    IEC104_Log("IEC104: ClockSync rejected (clock update failed)\r\n");
    return false;
  }

  synchronized_at_tick = HAL_GetTick();
  clock_synchronized = true;

  IEC104_Log("IEC104: ClockSync accepted (software clock updated)\r\n");

  return true;
}

static bool DecodeCommand(CS101_ASDU asdu, ControlCommand *command,
                          bool *is_select)
{
  InformationObject object;

  if ((CS101_ASDU_getNumberOfElements(asdu) != 1) ||
      (CS101_ASDU_getCOT(asdu) != CS101_COT_ACTIVATION))
    return false;

  object = CS101_ASDU_getElement(asdu, 0);
  if (object == NULL)
    return false;

  command->ioa = (uint32_t)InformationObject_getObjectAddress(object);
  switch (CS101_ASDU_getTypeID(asdu))
  {
    case C_SC_NA_1:

      command->type = CONTROL_TYPE_SINGLE;
      command->value.single = SingleCommand_getState((SingleCommand)object);
      command->qualifier = SingleCommand_getQU((SingleCommand)object);
      *is_select = SingleCommand_isSelect((SingleCommand)object);
      break;

    case C_DC_NA_1:

      command->type = CONTROL_TYPE_DOUBLE;
      command->value.double_state = DoubleCommand_getState((DoubleCommand)object);
      command->qualifier = DoubleCommand_getQU((DoubleCommand)object);
      *is_select = DoubleCommand_isSelect((DoubleCommand)object);
      break;

    case C_RC_NA_1:

      command->type = CONTROL_TYPE_STEP;
      command->step_direction = (uint8_t)StepCommand_getState((StepCommand)object);
      command->qualifier = StepCommand_getQU((StepCommand)object);
      *is_select = StepCommand_isSelect((StepCommand)object);
      break;

    case C_SE_NA_1:

      command->type = CONTROL_TYPE_NORMALIZED;
      command->value.normalized = SetpointCommandNormalized_getValue((SetpointCommandNormalized)object);
      command->qualifier = SetpointCommandNormalized_getQL((SetpointCommandNormalized)object);
      *is_select = SetpointCommandNormalized_isSelect((SetpointCommandNormalized)object);
      break;

    case C_SE_NB_1:

      command->type = CONTROL_TYPE_SCALED;
      command->value.scaled = SetpointCommandScaled_getValue((SetpointCommandScaled)object);
      command->qualifier = SetpointCommandScaled_getQL((SetpointCommandScaled)object);
      *is_select = SetpointCommandScaled_isSelect((SetpointCommandScaled)object);
      break;

    case C_SE_NC_1:

      command->type = CONTROL_TYPE_FLOAT;
      command->value.real = SetpointCommandShort_getValue((SetpointCommandShort)object);
      command->qualifier = SetpointCommandShort_getQL((SetpointCommandShort)object);
      *is_select = SetpointCommandShort_isSelect((SetpointCommandShort)object);
      break;

    default:
      InformationObject_destroy(object);
      return false;
  }

  InformationObject_destroy(object);

  return true;
}

static bool CommandASDU(void *parameter, IMasterConnection connection,
                        CS101_ASDU asdu)
{
  ControlCommand command = {0};
  PendingCommand *slot = NULL;
  bool is_select;
  bool accepted;
  size_t index;
  (void)parameter;

  switch (CS101_ASDU_getTypeID(asdu))
  {
    case C_SC_NA_1:
    case C_DC_NA_1:
    case C_RC_NA_1:
    case C_SE_NA_1:
    case C_SE_NB_1:
    case C_SE_NC_1:
      break;
    default:
      return false;
  }

  if (!IsActive(connection) || !DecodeCommand(asdu, &command, &is_select))
  {
    (void)IMasterConnection_sendACT_CON(connection, asdu, true);
    IEC104_Log("IEC104: command rejected type=%u (inactive or invalid ASDU)\r\n",
               (unsigned)CS101_ASDU_getTypeID(asdu));

    return true;
  }

  if (is_select)
  {
    accepted = ControlService_Select(&command, session_id);
    (void)IMasterConnection_sendACT_CON(connection, asdu, !accepted);

    IEC104_Log("IEC104: SELECT type=%u IOA=%lu %s\r\n",
               (unsigned)CS101_ASDU_getTypeID(asdu),
               (unsigned long)command.ioa,
               accepted ? "accepted" : "rejected");

    return true;
  }

  for (index = 0U; index < IEC104_PENDING_COMMANDS; index++)
  {
    if (!commands[index].occupied)
    {
      slot = &commands[index];
      break;
    }
  }

  if ((slot == NULL) || (CS101_ASDU_clone(asdu, &slot->request) == NULL))
  {
    (void)IMasterConnection_sendACT_CON(connection, asdu, true);
    IEC104_Log("IEC104: EXECUTE type=%u IOA=%lu rejected (pending full or clone failed)\r\n",
               (unsigned)CS101_ASDU_getTypeID(asdu),
               (unsigned long)command.ioa);

    return true;
  }

  next_transaction_id++;

  if (next_transaction_id == 0U)
    next_transaction_id++;

  if (!ControlService_Execute(&command, session_id, next_transaction_id))
  {
    (void)IMasterConnection_sendACT_CON(connection, asdu, true);
    IEC104_Log("IEC104: EXECUTE type=%u IOA=%lu rejected (selection or queue)\r\n",
               (unsigned)CS101_ASDU_getTypeID(asdu),
               (unsigned long)command.ioa);

    return true;
  }

  slot->transaction_id = next_transaction_id;
  slot->session_id = session_id;
  slot->connection = connection;
  slot->result_ready = false;
  slot->occupied = true;

  (void)IMasterConnection_sendACT_CON(connection, asdu, false);
  IEC104_Log("IEC104: EXECUTE type=%u IOA=%lu transaction=%lu accepted\r\n",
             (unsigned)CS101_ASDU_getTypeID(asdu),
             (unsigned long)command.ioa,
             (unsigned long)next_transaction_id);

  return true;
}

void IEC104_HandlersAttach(CS104_Slave slave)
{
  slave_instance = slave;
  active_connection = NULL;
  interrogation.active = false;
  spontaneous_pending = false;

  memset(commands, 0, sizeof(commands));

  CS104_Slave_setAllowedCAHandler(slave, AllowedCA, NULL);
  CS104_Slave_setConnectionEventHandler(slave, ConnectionEvent, NULL);
  CS104_Slave_setInterrogationHandler(slave, Interrogation, NULL);
  CS104_Slave_setCounterInterrogationHandler(slave, CounterInterrogation, NULL);
  CS104_Slave_setReadHandler(slave, ReadPoint, NULL);
  CS104_Slave_setClockSyncHandler(slave, ClockSync, NULL);
  CS104_Slave_setASDUHandler(slave, CommandASDU, NULL);
  CS104_Slave_setRawMessageHandler(slave, IEC104_LogRawMessage,
                                 CS104_Slave_getAppLayerParameters(slave));
}

void IEC104_HandlersDetach(void)
{
  ControlService_CancelSelection(session_id);
  slave_instance = NULL;
  active_connection = NULL;
  interrogation.active = false;
  spontaneous_pending = false;
  memset(commands, 0, sizeof(commands));
}

bool IEC104_HandlersHasActiveMaster(void)
{
  return active_connection != NULL;
}

bool IEC104_HandlersGetUtcTime(uint64_t *utc_ms)
{
  if ((utc_ms == NULL) || !clock_synchronized)
    return false;

  *utc_ms = Hal_getTimeInMs();
  return true;
}

void IEC104_HandlersProcess(void)
{
  ControlResult result;
  PointSnapshot event;
  size_t index;
  unsigned budget = 2U;

  while (ControlService_TakeResult(&result))
  {
    for (index = 0U; index < IEC104_PENDING_COMMANDS; index++)
    {
      PendingCommand *pending = &commands[index];
      if (pending->occupied &&
          (pending->transaction_id == result.transaction_id))
      {
        pending->result_ready = true;
        pending->result_success = result.success;
        break;
      }
    }
  }

  for (index = 0U; index < IEC104_PENDING_COMMANDS; index++)
  {
    PendingCommand *pending = &commands[index];
    
    CS101_ASDU response;
    bool sent;

    if (!pending->occupied || !pending->result_ready)
      continue;

    if ((pending->session_id != session_id) ||
        !IsActive(pending->connection))
    {
      pending->occupied = false;
      continue;
    }

    response = (CS101_ASDU)&pending->request;
    if (pending->result_success)
      sent = IMasterConnection_sendACT_TERM(pending->connection, response);
    else
    {
      CS101_ASDU_setCOT(response, CS101_COT_ACTIVATION_TERMINATION);
      CS101_ASDU_setNegative(response, true);
      sent = IMasterConnection_sendASDU(pending->connection, response);
    }

    if (sent)
    {
      IEC104_Log("IEC104: EXECUTE transaction=%lu %s\r\n",
                 (unsigned long)pending->transaction_id,
                 pending->result_success ? "success" : "failed");
      pending->occupied = false;
    }
  }

  if (interrogation.active &&
      (interrogation.session_id == session_id) &&
      IsActive(interrogation.connection))
  {
    while ((budget-- > 0U) &&
           (interrogation.next_index < interrogation.count))
    {
      if (!SendPoint(interrogation.connection,
                     &interrogation.points[interrogation.next_index],
                     CS101_COT_INTERROGATED_BY_STATION, false))
        break;
      interrogation.next_index++;
    }

    if (interrogation.next_index == interrogation.count)
    {
      bool sent = IMasterConnection_sendACT_TERM(interrogation.connection,
                                                (CS101_ASDU)&interrogation.request);
      if (sent)
      {
        interrogation.active = false;
        IEC104_Log("IEC104: GI complete points=%u\r\n",
                   (unsigned)interrogation.count);
      }
    }
  }

  /* Events are not retained across disconnects: a new master should request GI. */
  if (interrogation.active && IsActive(active_connection))
    return;

  for (index = 0U; index < 4U; index++)
  {
    if (spontaneous_pending)
      event = spontaneous_event;
    else if (!PointDatabase_TakeEvent(&event))
      break;

    if (!IsActive(active_connection))
    {
      spontaneous_pending = false;
      continue;
    }

    if (SendPoint(active_connection, &event, CS101_COT_SPONTANEOUS, true))
      spontaneous_pending = false;
    else
    {
      spontaneous_event = event;
      spontaneous_pending = true;
      break;
    }
  }
}
