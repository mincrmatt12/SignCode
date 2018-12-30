#include <HttpClient.h>

namespace util {

	struct Download {
		uint32_t status_code;
		char * buf;
		uint32_t length;
		bool error;
	};

	// Mallocs a buffer, and downloads.
	// Also occasionally runs the serial buffer.
	// Does not make a c_str, returns the length in a struct however. The response may be less than length if error is set.
	Download download_from(HttpClient& client, const char * host, const char * path, uint32_t timeout=500);
	Download download_from(HttpClient& client, const char * host, const char * path, const char * const headers[][2], uint32_t timeout=500);
}
