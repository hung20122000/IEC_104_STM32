#include "iec104/iec104_log.h"

#include "iec104/iec104_handlers.h"

#include <stdarg.h>
#include <stdio.h>

void IEC104_Log(const char *format, ...)
{
  uint64_t utc_ms;
  struct sCP56Time2a local_time;
  va_list args;

  if (IEC104_HandlersGetUtcTime(&utc_ms))
  {
    CP56Time2a_setFromMsTimestamp(
      &local_time, utc_ms + 7ULL * 60ULL * 60ULL * 1000ULL);

    printf("[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
           2000 + CP56Time2a_getYear(&local_time),
           CP56Time2a_getMonth(&local_time),
           CP56Time2a_getDayOfMonth(&local_time),
           CP56Time2a_getHour(&local_time),
           CP56Time2a_getMinute(&local_time),
           CP56Time2a_getSecond(&local_time),
           CP56Time2a_getMillisecond(&local_time));
  }
  else
    printf("[UNSYNC] ");

  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

void IEC104_LogRawMessage(void *parameter, IMasterConnection connection,
                         uint8_t *message, int message_size, bool sent)
{
#if IEC104_LOG_FRAMES_ENABLED
  CS101_AppLayerParameters parameters = parameter;
  const char *direction = sent ? "TX" : "RX";
  
  /* lib60870 invokes the TX hook before Socket_write; RX precedes validation. */
  const char *stage = sent ? "socket-attempt" : "received";
  (void)connection;

  if ((message == NULL) || (message_size <= 0))
    return;

  if ((message_size < 6) || (message[0] != 0x68) ||
      ((int)message[1] + 2 != message_size))
  {
    IEC104_Log("IEC104 %s: invalid APDU length=%d stage=%s\r\n",
               direction, message_size, stage);
  }
  else
  {
    unsigned ns = ((unsigned)message[2] |
                   ((unsigned)message[3] << 8)) >> 1;
    unsigned nr = ((unsigned)message[4] |
                   ((unsigned)message[5] << 8)) >> 1;

    if ((message[2] & 1U) == 0U)
    {
      if ((parameters == NULL) ||
          (parameters->sizeOfTypeId != 1) ||
          (parameters->sizeOfVSQ != 1) ||
          (parameters->sizeOfCOT < 1) || (parameters->sizeOfCOT > 2) ||
          (parameters->sizeOfCA < 1) || (parameters->sizeOfCA > 2) ||
          (parameters->sizeOfIOA < 1) || (parameters->sizeOfIOA > 3))
      {
        IEC104_Log("IEC104 %s I: N(S)=%u N(R)=%u ASDU=unparsed "
                   "(invalid parameters) stage=%s\r\n",
                   direction, ns, nr, stage);
      }
      else
      {
        int header_size = 2 + parameters->sizeOfCOT + parameters->sizeOfCA;

        if (message_size < 6 + header_size)
        {
          IEC104_Log("IEC104 %s I: N(S)=%u N(R)=%u ASDU=truncated stage=%s\r\n",
                     direction, ns, nr, stage);
        }
        else
        {
          const uint8_t *asdu = message + 6;
          int ca_offset = 2 + parameters->sizeOfCOT;
          unsigned ca = asdu[ca_offset];
          int32_t first_ioa = -1;
          int index;

          if (parameters->sizeOfCA == 2)
            ca |= (unsigned)asdu[ca_offset + 1] << 8;

          if (((asdu[1] & 0x7fU) != 0U) &&
              (message_size >= 6 + header_size + parameters->sizeOfIOA))
          {
            first_ioa = 0;
            for (index = 0; index < parameters->sizeOfIOA; index++)
              first_ioa |= (int32_t)asdu[header_size + index] << (8 * index);
          }

          IEC104_Log("IEC104 %s I ASDU: N(S)=%u N(R)=%u type=%u COT=%u "
                     "CA=%u N=%u SQ=%u firstIOA=%ld negative=%u test=%u stage=%s\r\n",
                     direction, ns, nr, (unsigned)asdu[0],
                     (unsigned)(asdu[2] & 0x3fU), ca,
                     (unsigned)(asdu[1] & 0x7fU), (unsigned)(asdu[1] >> 7),
                     (long)first_ioa, (unsigned)((asdu[2] >> 6) & 1U),
                     (unsigned)(asdu[2] >> 7), stage);
        }
      }
    }
    else if ((message[2] & 3U) == 1U)
    {
      IEC104_Log("IEC104 %s S: N(R)=%u stage=%s\r\n", direction, nr, stage);
    }
    else
    {
      const char *name;

      switch (message[2])
      {
        case 0x07: name = "STARTDT_ACT"; break;
        case 0x0b: name = "STARTDT_CON"; break;
        case 0x13: name = "STOPDT_ACT";  break;
        case 0x23: name = "STOPDT_CON";  break;
        case 0x43: name = "TESTFR_ACT";  break;
        case 0x83: name = "TESTFR_CON";  break;
        default:   name = "UNKNOWN";    break;
      }

      IEC104_Log("IEC104 %s U: %s control=0x%02X stage=%s\r\n",
                 direction, name, (unsigned)message[2], stage);
    }
  }

#if IEC104_LOG_FRAME_HEX_ENABLED
  IEC104_Log("IEC104 %s APDU:", direction);
  for (int index = 0; index < message_size; index++)
    printf(" %02X", (unsigned)message[index]);
  printf("\r\n");
#endif
#else
  (void)parameter;
  (void)connection;
  (void)message;
  (void)message_size;
  (void)sent;
#endif
}
