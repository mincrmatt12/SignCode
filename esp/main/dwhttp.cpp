#include "dwhttp.h"
#include "esp_log.h"
#include <esp_system.h>
#include <cstdlib>
#include <string.h>
#include <bearssl.h>

#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <vector>
extern "C" {
#include <http_client.h>
};

#include "sd.h"
#include <memory>

#undef connect
#undef read
#undef write
#undef socket
#undef send
#undef recv
#undef close
#undef bind

// shitty HTTP client....
const char *msign_ua = "MSign/4.0.0 ESP8266 screwanalytics/1.0";
static const char *TAG = "dwhttp";

namespace dwhttp {
	namespace detail {
		const static char * TAG = "d_impl";

		namespace adapter {
			const static char * TAG = "d_adapter";
			struct HttpAdapter {
				bool connect(const char *host, const char* port="80") {
					if (is_connected()) return false;

					addrinfo hints{};
					addrinfo *result{}, *rp{};
					hints.ai_family = AF_INET;
					hints.ai_socktype = SOCK_STREAM;

					int stat;
					if ((stat = lwip_getaddrinfo(host, port, &hints, &result))) {
						ESP_LOGE(TAG, "gai fail: %s/%d", lwip_strerr(stat), stat);
					}

					for (rp = result; rp != nullptr; rp = rp->ai_next) {
						sockno = lwip_socket(rp->ai_family, rp->ai_socktype,
								rp->ai_protocol);
						if (sockno == -1)
							continue;

						if (lwip_connect(sockno, rp->ai_addr, rp->ai_addrlen) != -1)
							break;                  /* Success */

						lwip_close(sockno);
						sockno = -1;
					}

					lwip_freeaddrinfo(result);

					if (rp == nullptr) {
						ESP_LOGE(TAG, "failed to lookup %s", host);
						return false;
					}

					// Set a 5 second rcvtimeo 
					const struct timeval timeout = { 5, 0 };
					setsockopt(sockno, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

					// We are now connected.
					return true;
				}

				size_t read(uint8_t *buf, size_t max) {
					int code = lwip_recv(sockno, buf, max, 0);
					if (code >= 0) return code;
					else {
						switch (errno) {
							default:
								lwip_close(sockno);
							case EBADF:
								sockno = -1;
								return -1;
						}
					}
				}
				bool write(const uint8_t *buf, size_t length) {
					size_t pos = 0;
					while (pos < length) {
						int code = lwip_send(sockno, buf + pos, length - pos, 0);
						if (code >= 0) {
							pos += code;
						}
						else {
							switch (errno) {
								default:
									lwip_close(sockno);
								case EBADF:
									sockno = -1;
									return false;
							}
						}
					}
					return true;
				}

				bool write(const char *buf) {
					return write((const uint8_t *)buf, strlen(buf));
				}

				void close() {
					if (sockno != -1) lwip_close(sockno);
					sockno = -1;
				}

				void flush() {}

				bool is_connected() {
					return sockno != -1;
				}
			protected:
				int sockno = -1;
			};

			struct HeldTA {
				HeldTA(const char *filename) {
					std::unique_ptr<br_x509_decoder_context> ctx(new br_x509_decoder_context{});
					std::unique_ptr<br_sha256_context> hashet(new br_sha256_context{});

					br_sha256_init(hashet.get());
					br_x509_decoder_init(ctx.get(), (void (*)(void *, const void *, size_t))br_sha256_update, hashet.get());
					FIL certf;

					// Open the file (it's assumed it already exists at this point)
					f_open(&certf, filename, FA_READ);

					// Continue reading in 32 byte chunks
					char buf[32];
					while (!f_eof(&certf)) {
						UINT btr = 0;
						if (f_read(&certf, buf, 32, &btr)) {
							f_close(&certf);
							ESP_LOGE(TAG, "File access in HeldTA failed");
							return;
						}
						br_x509_decoder_push(ctx.get(), buf, btr);
					}

					f_close(&certf);

					br_x509_pkey *pk = br_x509_decoder_get_pkey(ctx.get());
					if (pk == nullptr) {
						ESP_LOGE(TAG, "No pkey in cert!");
						return;
					}

					// Initialize the dn in the TA
					anchor.dn.data = (uint8_t *)malloc(64);
					if (!anchor.dn.data) {
						ESP_LOGE(TAG, "OOM allocating VDN");
						return;
					}
					br_sha256_out(hashet.get(), anchor.dn.data);
					anchor.dn.len = 64;
					anchor.flags = br_x509_decoder_isCA(ctx.get()) ? BR_X509_TA_CA : 0;

					// Extract the public key
					switch (pk->key_type) {
						case BR_KEYTYPE_RSA:
							anchor.pkey.key_type = BR_KEYTYPE_RSA;
							anchor.pkey.key.rsa.n = (uint8_t*)malloc(pk->key.rsa.nlen);
							anchor.pkey.key.rsa.e = (uint8_t*)malloc(pk->key.rsa.elen);
							if ((anchor.pkey.key.rsa.n == nullptr) || (anchor.pkey.key.rsa.e == nullptr)) {
								ESP_LOGE(TAG, "Malloc key elements failed");
								return;
							}
							memcpy(anchor.pkey.key.rsa.n, pk->key.rsa.n, pk->key.rsa.nlen);
							anchor.pkey.key.rsa.nlen = pk->key.rsa.nlen;
							memcpy(anchor.pkey.key.rsa.e, pk->key.rsa.e, pk->key.rsa.elen);
							anchor.pkey.key.rsa.elen = pk->key.rsa.elen;
							is_ok = true;
							return;
						case BR_KEYTYPE_EC:
							anchor.pkey.key_type = BR_KEYTYPE_EC;
							anchor.pkey.key.ec.curve = pk->key.ec.curve;
							anchor.pkey.key.ec.q = (uint8_t*)malloc(pk->key.ec.qlen);
							if (anchor.pkey.key.ec.q == nullptr) {
								ESP_LOGE(TAG, "Malloc key elements failed");
								return;
							}
							memcpy(anchor.pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
							anchor.pkey.key.ec.qlen = pk->key.ec.qlen;
							is_ok = true;
							return;
						default:
							ESP_LOGE(TAG, "Unknown key type");
							return;
					}
				}
				~HeldTA() {
					ESP_LOGD(TAG, "Freeing TA");
					free(anchor.dn.data);
					if (anchor.pkey.key_type == BR_KEYTYPE_RSA) {
						free(anchor.pkey.key.rsa.n);
						free(anchor.pkey.key.rsa.e);
					} else if (anchor.pkey.key_type == BR_KEYTYPE_EC) {
						free(anchor.pkey.key.ec.q);
					}
				}

				HeldTA(const HeldTA& other) = delete;

				// valid for duration of this class and when is_ok is true
				const br_x509_trust_anchor * get_anchor() {
					if (is_ok) return &anchor;
					else return nullptr;
				}

			private:
				br_x509_trust_anchor anchor{};
				bool is_ok = false;
			};

			// These are kept as staticly allocated to avoid heap fragmentation.
			// The rest of the context is freed as necessary (as it technically isn't required while the HTTP one is running)
			//
			// Placed in IRAM to avoid putting unnecessary pressure on DRAM heap to allow more tasks to be allocated
			IRAM_ATTR uint8_t ssl_io_buffer_in[BR_SSL_BUFSIZE_INPUT];
			uint8_t ssl_io_buffer_out[1024 + 85]; // 1K out buffer + 85 bytes overhead

			struct HttpsAdapter : HttpAdapter {
				bool connect(const char* host) {
					// Try and establish a connection with a socket.
					if (!HttpAdapter::connect(host, "443")) return false;
					// Create all the objects
					ssl_cc = new (std::nothrow) br_ssl_client_context{};
					if (!ssl_cc) {
						ESP_LOGE(TAG, "Out of memory allocating client_context");
						return false;
					}
					ssl_xc = new (std::nothrow) br_x509_minimal_context{};
					if (!ssl_xc) {
						ESP_LOGE(TAG, "Out of memory allocating minimal_context");
						delete ssl_cc; ssl_cc = nullptr;
						return false;
					}
					ssl_ic = new (std::nothrow) br_sslio_context{};
					if (!ssl_ic) {
						ESP_LOGE(TAG, "Out of memory allocating io_context");
						delete ssl_cc; ssl_cc = nullptr;
						delete ssl_xc; ssl_xc = nullptr;
						return false;
					}

					// Begin initializing bearssl
					br_ssl_client_init_full(ssl_cc, ssl_xc, nullptr, 0);
					// Set the buffers
					br_ssl_engine_set_buffers_bidi(&ssl_cc->eng, ssl_io_buffer_in, sizeof ssl_io_buffer_in, ssl_io_buffer_out, sizeof ssl_io_buffer_out);
					// Feed some entropy into bearssl
					uint32_t block[8];
					for (int i = 0; i < 8; ++i) block[i] = esp_random();
					br_ssl_engine_inject_entropy(&ssl_cc->eng, &block, sizeof block);
					// Setup certificate verification with our dynamic thingies
					br_x509_minimal_set_dynamic(ssl_xc, this,
						(const br_x509_trust_anchor * (*)(void *, void*, size_t))(&HttpsAdapter::find_hashed_ta), 
						(void (*)(void *, const br_x509_trust_anchor *))(&HttpsAdapter::held_ta_free)
					);
					// Copy the host
					active_host = strdup(host);
					// Reset the SSL context
					br_ssl_client_reset(ssl_cc, active_host, 0);
					// Initialize IO
					br_sslio_init(ssl_ic, &ssl_cc->eng, 
						(int (*)(void *, unsigned char *, size_t))(&HttpsAdapter::_read), this,
						(int (*)(void *, const unsigned char *, size_t))(&HttpsAdapter::_write), this
					);
					return true;
				}

				size_t read(uint8_t *buf, size_t max) {
					if (!is_connected()) return -1;
					int rlen = br_sslio_read_all(ssl_ic, buf, max);
					if (rlen < 0) {
						close();
						return -1;
					}
					return max;
				}

				bool write(const uint8_t *buf, size_t length) {
					if (!is_connected()) return false;
					int rlen = br_sslio_write_all(ssl_ic, buf, length);
					if (rlen < 0) {
						close();
						return false;
					}
					return true;
				}

				bool write(const char *buf) {
					return write((const uint8_t *)buf, strlen(buf));
				}

				void close() {
					if (is_connected()) {
						// Check error for logging
						int err = br_ssl_engine_last_error(&ssl_cc->eng);
						if (err) ESP_LOGW(TAG, "ssl closing with error %d", err);
						// Send a close, but ignore it
						br_sslio_close(ssl_ic);
						// Close the underlying socket
						HttpAdapter::close();
					}
					// Delete all the bearssl objects
					delete ssl_cc;
					delete ssl_xc;
					delete ssl_ic;
					delete held_ta; // should be freed earlier
					free(active_host);
					ssl_cc = nullptr;
					ssl_xc = nullptr;
					ssl_ic = nullptr;
					active_host = nullptr;
					held_ta = nullptr;
				}

				void flush() {
					if (!is_connected()) return;
					br_sslio_flush(ssl_ic);
				}

				bool is_connected() {
					return ssl_cc && ssl_xc && ssl_ic;
				}
			private:
				int _read(unsigned char *buf, size_t len) {
					while (true) {
						ssize_t rlen;

						rlen = lwip_recv(this->sockno, buf, len, 0);
						if (rlen <= 0) {
							if (rlen < 0 && errno == EINTR) {
								continue;
							}
							return -1;
						}
						return (int)rlen;
					}
				}

				int _write(const unsigned char *buf, size_t len) {
					while (true) {
						ssize_t wlen;

						wlen = lwip_send(this->sockno, buf, len, 0);
						if (wlen <= 0) {
							if (wlen < 0 && errno == EINTR) continue;
							return -1;
						}
						return (int)wlen;
					}
				}

				const br_x509_trust_anchor * find_hashed_ta(void *hashed_dn, size_t len) {
					if (held_ta) {
						ESP_LOGW(TAG, "didn't free held_ta");
						delete held_ta;
						held_ta = nullptr;
					}

					const static char* prefix = "0:/ca/";

					char buf[256];
					memset(buf, 0, 256);
					strcpy(buf, prefix);

					size_t off = strlen(prefix);

					// Write the hashed_dn as a hex str into the buf
					for (size_t i = 0; i < len; ++i) {
						if (off >= 255) {
							ESP_LOGE(TAG, "hashed_dn is too long to fit in the fn buffer!");
							return nullptr;
						}
						else {
							snprintf(buf + off, 3, "%02x", *((uint8_t *)(hashed_dn) + i));
							off += 2;
						}
					}

					// Check if that file exists
					if (f_stat(buf, nullptr) != FR_OK) {
						ESP_LOGD(TAG, "couldn't find CA file %s", buf);
						return nullptr;
					}

					// Initialize the thing
					held_ta = new HeldTA(buf);
					auto ta = held_ta->get_anchor();
					ESP_LOGD(TAG, "Got trust anchor %p", ta);
					return ta;
				}

				// free
				void held_ta_free(const br_x509_trust_anchor *ta) {
					if (!held_ta) {
						ESP_LOGW(TAG, "unknown held_ta");
					}
					delete held_ta;
					held_ta = nullptr;
				}

				// These are dynamically allocated when a connection is made
				br_ssl_client_context *ssl_cc = nullptr;
				br_x509_minimal_context *ssl_xc = nullptr;
				br_sslio_context *ssl_ic = nullptr;
				char * active_host = nullptr;

				// This is dynamically allocated when requested.
				HeldTA *held_ta = nullptr;
			};
		}

		template<typename Adapter>
		struct Downloader {
			// helper for char-by-char access
			//
			// will this perform terribly? perhaps!
			int16_t next() {
				uint8_t buf;
				if (!read_from(&buf, 1)) return -1;
				return buf;
			}

			size_t read_from(uint8_t *buf, size_t max) {
				if (!socket.is_connected()) return 0;
				int r = socket.read(buf, max);
				if (r <= 0) {
					ESP_LOGW(TAG, "Read failed.");
					socket.close();
					return 0;
				}
				return r;
			}

			// Close sockets
			void stop() {
				socket.close();
			}

			inline const int& result_code() const {
				return state.c.result_code;
			}

			inline const int& content_length() const {
				return state.c.content_length;
			}

			inline bool is_unknown_length() const {
				return state.c.connection == HTTP_CLIENT_CONNECTION_CLOSE;
			}

			bool request(const char *host, const char *path, const char* method, const char * const headers[][2], const uint8_t * body=nullptr, const size_t bodylen=0) {
				if (socket.is_connected()) {
					ESP_LOGW(TAG, "Socket wasn't closed, closing");
					socket.close();
				}
				if (!socket.connect(host)) {
					ESP_LOGE(TAG, "Failed to connect to host");
					return false;
				}
				
				// Send request
				if (!(socket.write(method) && 
					socket.write(" ") &&
					socket.write(path) &&
					socket.write(" HTTP/1.1\r\n"))) {
					ESP_LOGE(TAG, "Failed to send request path");

					socket.close();
					return false;
				}

				// Send headers
				if (!(write_header("Host", host) &&
					  write_header("User-Agent", msign_ua))) {

					ESP_LOGE(TAG, "Failed to send request headers");

					socket.close();
					return false;
				}

				// Send body length if present
				if (body) {
					char buf[32];
					snprintf(buf, 32, "%d", (int)bodylen);
					if (!write_header("Content-Length", buf)) {
						ESP_LOGE(TAG, "Failed to send body headers");

						socket.close();
						return false;
					}
				}

				// Write all provided headers
				for (int i = 0;;++i) {
					if (headers[i][0] == nullptr || headers[i][1] == nullptr) break;
					if (!write_header(headers[i][0], headers[i][1])) {
						ESP_LOGE(TAG, "Failed to send user headers");

						socket.close();
						return false;
					}
					ESP_LOGD(TAG, "hdr: %s --> %s", headers[i][0], headers[i][1]);
				}

				socket.write("\r\n");
				if (body) {
					if (!socket.write(body, bodylen)) {
						ESP_LOGE(TAG, "Failed to send body");

						socket.close();
						return false;
					}
				}

				socket.flush();

				// Initialize http request parser
				http_client_start(&state);

				while (true) {
					uint8_t buf;
					int recvd_bytes = socket.read(&buf, 1);
					if (recvd_bytes < 0) {
						ESP_LOGE(TAG, "Got error while recving");
						socket.close();
						return false;
					}
					// Otherwise, feed it into nmfu
					switch (http_client_feed(buf, false, &state)) {
						case HTTP_CLIENT_OK:
							continue;
						case HTTP_CLIENT_FAIL:
							ESP_LOGE(TAG, "Parser failed");
							socket.close();
							return false;
						case HTTP_CLIENT_DONE:
							ESP_LOGD(TAG, "Finished parsing req headers");
							goto finish_req;
					}
				}
finish_req:
				ESP_LOGD(TAG, "Ready with code = %d; length = %d; unklen = %d", result_code(), content_length(), is_unknown_length());
				return true;
			}
		private:
			Adapter socket;
			http_client_state_t state;

			bool write_header(const char * name, const char * value) {
				if (!socket.write(name)) return false;
				if (!socket.write(": ")) return false;
				if (!socket.write(value)) return false;
				if (!socket.write("\r\n")) return false;
				return true;
			}
		};

		Downloader<adapter::HttpAdapter> dwnld;
		Downloader<adapter::HttpsAdapter> dwnld_s;

		template<typename T>
		inline constexpr Downloader<T>& get_downloader() {return dwnld;}; 

		template<>
		inline constexpr Downloader<adapter::HttpsAdapter>& get_downloader() {return dwnld_s;};

	}
}

template<typename T>
inline dwhttp::Download download_from_impl(const char *host, const char *path, const char * const headers[][2], const char * method, const char * body) {
	constexpr static dwhttp::detail::Downloader<T>& dwnld = dwhttp::detail::get_downloader<T>();
	dwhttp::Download d;
	if (!dwnld.request(host, path, method, headers, (const uint8_t *)body, body ? strlen(body) : 0)) {
		d.error = true;
		return d;
	}

	d.status_code = dwnld.result_code();
	d.error = false;
	if (dwnld.result_code() < 200 || dwnld.result_code() >= 300) {
		dwnld.stop();
		ESP_LOGE(TAG, "download_from_impl got result code %d", d.status_code);
		d.error = true;
		return d;
	}

	if (!dwnld.is_unknown_length()) {
		d.length = dwnld.content_length();
		d.buf = (char *)malloc(d.length);
		// TODO: rewrite me please!
		for (int32_t i = 0; i < d.length; ++i) {
			auto x = dwnld.next();
			if (x != -1) {
				d.buf[i] = (char)x;
			}
			else {
				free(d.buf);
				d.error = true;
				return d;
			}
		}
	}
	else {
		// download until the connection ends
		d.length = 128;
		d.buf = (char *)malloc(d.length);
		size_t i = 0;
		int16_t x;
		while ((x = dwnld.next()) != -1) {
			if (i == d.length) {
				d.length += 128;
				d.buf = (char *)realloc(d.buf, d.length);
				if (d.buf == nullptr) {
					d.error = true;
					return d;
				}
			}
			d.buf[i++] = x;
		}
		d.buf = (char *)realloc(d.buf, i);
		d.length = i;
	}

	return d;
}

dwhttp::Download dwhttp::download_from(const char *host, const char *path, const char * const headers[][2], const char * method, const char * body) {
	if (host[0] != '_') {
		return ::download_from_impl<detail::adapter::HttpAdapter>(host, path, headers, method, body);
	}
	else {
		return ::download_from_impl<detail::adapter::HttpsAdapter>(++host, path, headers, method, body);
	}
}

dwhttp::Download dwhttp::download_from(const char *host, const char *path) {
	const char * const headers[][2] = {{nullptr, nullptr}};
	return download_from(host, path, headers);
}

dwhttp::Download dwhttp::download_from(const char * host, const char * path, const char * const headers[][2]) {
	return download_from(host, path, headers, "GET");
}


template<typename T>
inline std::function<int16_t (void)> download_with_callback_impl(const char * host, const char * path, const char * const headers[][2], const char * method, const char * body, int16_t &status_code_out, int32_t &size_out) {
	static auto& dwnld = dwhttp::detail::get_downloader<T>();
	dwnld.request(host, path, method, headers, (const uint8_t *)body, body ? strlen(body) : 0);

	status_code_out = dwnld.result_code();
	size_out = dwnld.content_length();

	return std::bind(&dwhttp::detail::Downloader<T>::next, &dwnld);
}

std::function<int16_t (void)> dwhttp::download_with_callback(const char * host, const char * path, const char * const headers[][2], const char * method, const char * body, int16_t &status_code_out, int32_t &size_out) {
	if (host[0] != '_') {
		return ::download_with_callback_impl<detail::adapter::HttpAdapter>(host, path, headers, method, body, status_code_out, size_out);
	}
	else {
		return ::download_with_callback_impl<detail::adapter::HttpsAdapter>(++host, path, headers, method, body, status_code_out, size_out);
	}
}

std::function<int16_t (void)> dwhttp::download_with_callback(const char * host, const char * path) {
	const char * const headers[][2] = {{nullptr, nullptr}};
	return download_with_callback(host, path, headers);
}
std::function<int16_t (void)> dwhttp::download_with_callback(const char * host, const char * path, const char * const headers[][2]) {
	int16_t st;
	return download_with_callback(host, path, headers, st);
}
std::function<int16_t (void)> dwhttp::download_with_callback(const char * host, const char * path, int16_t &status_code_out) {
	const char * const headers[][2] = {{nullptr, nullptr}};
	return download_with_callback(host, path, headers, status_code_out);
}
std::function<int16_t (void)> dwhttp::download_with_callback(const char * host, const char * path, const char * const headers[][2], int16_t &status_code_out) {
	int32_t so;
	return download_with_callback(host, path, headers, "GET", nullptr, status_code_out, so);
}
std::function<int16_t (void)> dwhttp::download_with_callback(const char * host, const char * path, const char * const headers[][2], const char * method, const char * body) {
	int16_t sco;
	int32_t so;
	return download_with_callback(host, path, headers, method, body, sco, so);
}

void dwhttp::stop_download() {
	detail::dwnld.stop();
	detail::dwnld_s.stop();
}
