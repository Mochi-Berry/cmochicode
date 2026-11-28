#ifdef _WIN32
#define CMC_EXPORT __declspec(dllexport)
#else
#define CMC_EXPORT __attribute__((visibility("default")))
#endif

CMC_EXPORT int g_counter = 40;

CMC_EXPORT int add(int a, int b) { return a + b; }
CMC_EXPORT const char *version(void) { return "demoffi 1.0"; }
CMC_EXPORT double scale(double a, double b) { return a * b; }
