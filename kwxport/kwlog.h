
#if !defined(kwlog_h)
#define kwlog_h

void LogBegin(char const *baseName, bool suppressPrompts);
void LogEnd();
void LogPrint(char const *fmt, ...);
void LogError(char const *fmt, ...);

#endif  //  kwlog_h

