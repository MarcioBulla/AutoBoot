#include <stddef.h>

/* ESP32-S2's ROM str[n]casecmp uses Newlib locale/reentrancy state which
 * is incompatible with Picolibc. HTTP/WebSocket tokens use ASCII; keep
 * the C-locale comparison here and redirect callers with --wrap.
 */
static unsigned char ascii_lower(unsigned char value)
{
	return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

int __wrap_strncasecmp(const char *left, const char *right, size_t length)
{
	for (size_t i = 0; i < length; ++i) {
		unsigned char a = ascii_lower((unsigned char)left[i]);
		unsigned char b = ascii_lower((unsigned char)right[i]);

		if (a != b || a == 0) {
			return (int)a - (int)b;
		}
	}
	return 0;
}

int __wrap_strcasecmp(const char *left, const char *right)
{
	while (1) {
		unsigned char a = ascii_lower((unsigned char)*left++);
		unsigned char b = ascii_lower((unsigned char)*right++);

		if (a != b || a == 0) {
			return (int)a - (int)b;
		}
	}
}
