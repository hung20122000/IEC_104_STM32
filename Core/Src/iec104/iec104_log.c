#include "iec104/iec104_log.h"

#include <stdarg.h>
#include <stdio.h>

void IEC104_Log(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

void IEC104_LogTxASDU(CS101_ASDU asdu, int32_t ioa, bool sent)
{
#if IEC104_LOG_TX_ASDU_ENABLED
  const char *cause_name;

  switch (CS101_ASDU_getCOT(asdu))
  {
    case CS101_COT_ACTIVATION_CON:
      cause_name = "ACT_CON";
      break;
    case CS101_COT_ACTIVATION_TERMINATION:
      cause_name = "ACT_TERM";
      break;
    case CS101_COT_REQUEST:
      cause_name = "REQUEST";
      break;
    case CS101_COT_UNKNOWN_IOA:
      cause_name = "UNKNOWN_IOA";
      break;
    default:
      cause_name = "OTHER";
      break;
  }

  if (ioa >= 0)
    IEC104_Log("IEC104 TX ASDU: type=%u COT=%u(%s) CA=%u IOA=%ld negative=%u send=%s\r\n",
           (unsigned)CS101_ASDU_getTypeID(asdu),
           (unsigned)CS101_ASDU_getCOT(asdu), cause_name,
           (unsigned)CS101_ASDU_getCA(asdu), (long)ioa,
           CS101_ASDU_isNegative(asdu) ? 1U : 0U,
           sent ? "ok" : "failed");
  else
    IEC104_Log("IEC104 TX ASDU: type=%u COT=%u(%s) CA=%u IOA=? negative=%u send=%s\r\n",
           (unsigned)CS101_ASDU_getTypeID(asdu),
           (unsigned)CS101_ASDU_getCOT(asdu), cause_name,
           (unsigned)CS101_ASDU_getCA(asdu),
           CS101_ASDU_isNegative(asdu) ? 1U : 0U,
           sent ? "ok" : "failed");
#else
  (void)asdu;
  (void)ioa;
  (void)sent;
#endif
}
