#include "webui.h"
#include "config.h"
#include "esp_log.h"
#include "serial.h"
#include "common/util.h"
#include "ff.h"
#include "upd.h"
#include "wifitime.h"

extern "C" {
#include <http_serve.h>
#include <multipart_header.h>
}

#include <b64/cencode.h>

#include <lwip/sockets.h>
#include <esp_system.h>

#undef connect
#undef read
#undef write
#undef socket
#undef send
#undef recv
#undef close
#undef bind

// config defaults
#define DEFAULT_USERNAME "admin"
#define DEFAULT_PASSWORD "admin"

const static char * TAG = "webui";

namespace webui {
	int etag_num;

	// server
	int client_sock = -1;
	int server_sock = -1;

	http_serve_state_t * reqstate = nullptr;

	int print_to_client(const char* buf) {
		size_t pos = 0;
		size_t length = strlen(buf);
		while (pos < length) {
			int code = lwip_send(client_sock, buf + pos, length - pos, 0);
			if (code >= 0) {
				pos += code;
			}
			else {
				ESP_LOGE(TAG, "Failed to write to client %s b.c. (%d)", buf, errno);
				return -1;
			}
		}
		return pos;
	}

	bool check_auth(const char *in) {
		const char *user = config::manager.get_value(config::CONFIG_USER, DEFAULT_USERNAME);
		const char *pass = config::manager.get_value(config::CONFIG_PASS, DEFAULT_PASSWORD);

		char actual_buf[64] = {0};
		char *c = actual_buf;

		base64_encodestate es;
		base64_init_encodestate(&es);

		c += base64_encode_block(user, strlen(user), c, &es);
		c += base64_encode_block(":", 1, c, &es);
		c += base64_encode_block(pass, strlen(pass), c, &es);
		c += base64_encode_blockend(c, &es);
		*c = 0;

		return (strncmp(in, actual_buf, 64) == 0);
	}

	template<typename HdrT=char>
	void send_static_response(int code, const char * code_msg, const char * pstr_msg, const HdrT * extra_hdrs=nullptr) {
		int length = strlen(pstr_msg);

		char inbuf[16];

		print_to_client("HTTP/1.1 ");
		snprintf(inbuf, 16, "%d ", code);
		print_to_client(inbuf);
		print_to_client(code_msg);
		if (extra_hdrs) {
			print_to_client("\r\n");
			print_to_client(extra_hdrs);
		}
		print_to_client("\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: " );
		snprintf(inbuf, 16, "%d\r\n\r\n", length);
		print_to_client(inbuf);
		print_to_client(pstr_msg);
	}

	void stream_file(FIL* f) {
		char sendbuf[128];
		print_to_client("Content-Length: ");
		snprintf(sendbuf, 128, "%u\r\n\r\n", f_size(f));
		print_to_client(sendbuf);

		while (!f_eof(f)) {
			UINT chunkSize;
			if (f_read(f, sendbuf, 128, &chunkSize)) {
				ESP_LOGE(TAG, "failed to read from file");
			}
			if (lwip_send(client_sock, sendbuf, chunkSize, 0) < 0) {
				ESP_LOGE(TAG, "client closed conn/fail");
				return;
			}
		}
	}

	// AD-HOC MULTIPART/FORM-DATA PARSER
	//
	// Does contain an NMFU-based parser for the headers though
	//
	// Structure as a loop of
	//  - wait for \r\n--
	//  - using the helper function to bail early read and compare to the boundary. if at any point we fail go back to step 1 without eating
	//  - start the content-disposition NMFU (this sets up in a state struct which is passed in)
	//  - it eats until the body
	//  - if there's a content length, read that many
	//  - otherwise, read until \r\n-- again
	//
	// Takes a hook with the following "virtual" typedef
	// Size == -1 means start
	//      == -2 means end
	//
	// typedef bool (*MultipartHook)(uint8_t * tgt, int size, multipart_header_state_t * header_state);

	enum struct MultipartStatus {
		OK,
		INVALID_HEADER,
		NO_CONTENT_DISP,
		EOF_EARLY,
		HOOK_ABORT
	};

	int read_from_client(uint8_t * tgt, int size) {
		int i = 0;
		TickType_t started_at = xTaskGetTickCount();
		while (i < size) {
			if (started_at + pdMS_TO_TICKS(500) < xTaskGetTickCount()) {
				ESP_LOGE(TAG, "Failed to read client b.c. timeout");
				return -1;
			}
			int bytes_read = lwip_recv(client_sock, tgt + i, size - i, 0);
			if (bytes_read < 0) {
				ESP_LOGE(TAG, "Failed to read from client %d", errno);
				return -1;
			}
			tgt += bytes_read;
			i += bytes_read;
		}
		return i;
	}

	int read_from_req_body(uint8_t *tgt, int size) {
		// TODO: make me handle chunked encoding
		return read_from_client(tgt, size);
	}

	int read_from_req_body() {
		// TODO: make me handle chunked encoding
		uint8_t buf;
		read_from_req_body(&buf, 1);
		return buf;
	}

	template<typename MultipartHook>
	MultipartStatus do_multipart(MultipartHook hook) {
		multipart_header_state_t header_state;

continuewaiting:
		// Try to read the start of a header block
		while (true) {
			auto val = read_from_req_body();
			if (val == -1) break;
			if (val == '-') break;
			if (val == '\r') {
				if (read_from_req_body() != '\n') continue;
				if (read_from_req_body() != '-') continue;
				break;
			}
		}		
		ESP_LOGD(TAG, "got past -");
		if (errno) return MultipartStatus::EOF_EARLY;
		if (read_from_req_body() != '-') goto continuewaiting;
		ESP_LOGD(TAG, "got past -2");
		for (int i = 0; i < strlen(reqstate->c.multipart_boundary); ++i) {
			if (read_from_req_body() != reqstate->c.multipart_boundary[i]) goto continuewaiting;
		}

		ESP_LOGD(TAG, "Got multipart start");

		errno = 0;
		while (!errno) {
			// Start the multipart_header parser
			multipart_header_start(&header_state);

			// Continue parsing until done
			while (true) {
				if (errno) return MultipartStatus::EOF_EARLY;
				int x = read_from_req_body();
				switch (multipart_header_feed(x, x == -1, &header_state)) {
					case MULTIPART_HEADER_OK:
						continue;
					case MULTIPART_HEADER_FAIL:
						return MultipartStatus::INVALID_HEADER;
					case MULTIPART_HEADER_DONE:
						goto endloop;
				}
				continue;
endloop:
				break;
			}

			ESP_LOGD(TAG, "Got multipart header end");

			// Check if this is the end
			if (header_state.c.is_end) return MultipartStatus::OK;
			// Check if this is a valid header
			if (!header_state.c.ok) return MultipartStatus::NO_CONTENT_DISP;
			// Otherwise, start reading the response

			bool is_skipping = false;
			if (!hook(nullptr, -1, &header_state)) {
				is_skipping = true;
			}

			uint8_t buf[64];
			errno = 0;
			while (true) {
				if (errno) return MultipartStatus::EOF_EARLY;
				// Try to read
				int pos = 0;
				while (true) {
					if (pos == 64) {
						// Send that buffer into the hook
						if (!is_skipping) {
							if (!hook(buf, pos, &header_state)) return MultipartStatus::HOOK_ABORT;
						}
						pos = 0;
					}
					auto inval = read_from_req_body();
					if (inval == '\r') break;
					else {buf[pos++] = inval;}
				}
				// Send that buffer into the hook
				if (pos && !is_skipping) {
					if (!hook(buf, pos, &header_state)) return MultipartStatus::HOOK_ABORT;
				}
				buf[0] = '\r';
				pos = 1;
				// Try to read the rest of the boundary
				if ((buf[pos++] = read_from_req_body()) != '\n') goto flush_buf;
				if ((buf[pos++] = read_from_req_body()) != '-') goto flush_buf;
				if ((buf[pos++] = read_from_req_body()) != '-') goto flush_buf;
				for (int i = 0; i < strlen(reqstate->c.multipart_boundary); ++i) {
					if ((buf[pos++] = read_from_req_body()) != reqstate->c.multipart_boundary[i]) goto flush_buf;
				}
				// We have read an entire boundary delimiter, break out of this loop
				break;
flush_buf:
				if (!is_skipping) {
					if (!hook(buf, pos, &header_state)) return MultipartStatus::HOOK_ABORT;
				}
			}

			if (!is_skipping) hook(nullptr, -2, &header_state); // it's invalid to error in this case

			ESP_LOGD(TAG, "got end");
		}
		return MultipartStatus::OK;
	}

	void do_api_response(const char * tgt) {
		if (strcasecmp(tgt, "conf.txt") == 0) {
			if (reqstate->c.method == HTTP_SERVE_METHOD_GET) {
				FIL f; 
				if (f_open(&f, "0:/config.txt", FA_READ)) {
					send_static_response(500, "Internal Server Error", "Missing configuration.");
					return;
				}
				print_to_client("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\n");
				stream_file(&f);
				f_close(&f);
			}
			else if (reqstate->c.method == HTTP_SERVE_METHOD_POST) {
				// Verify there's actually a request body
				if (reqstate->c.content_length == 0) goto badrequest;

				uint8_t buf[64];

				FIL f; f_open(&f, "0:/config.txt.tmp", FA_CREATE_ALWAYS | FA_WRITE);
				for (int i = 0; i < reqstate->c.content_length; i += 64) {
					int len = read_from_req_body(buf, std::min(64, reqstate->c.content_length - i));
					UINT bw;
					if (f_write(&f, buf, len, &bw)) {
						ESP_LOGW(TAG, "Failed to dw config");
						send_static_response(500, "Internal Server Error", "Failed to write new config");
						f_close(&f);
						return;
					}
				}
				f_close(&f);
				f_unlink("0:/config.txt");
				f_rename("0:/config.txt.tmp", "0:/config.txt");
				config::manager.reload_config();
				// Send a handy dandy 204
				send_static_response(204, "No Content", "");
			}
			else goto invmethod;
		}
		else if (strcasecmp(tgt, "mpres.json") == 0) {
			if (reqstate->c.method != HTTP_SERVE_METHOD_GET) goto invmethod;
			bool m0 = f_stat("0:/model.bin", NULL) == 0;
			bool m1 = f_stat("0:/model1.bin", NULL) == 0;

			char buf[34] = {0};
			snprintf(buf, 34, "{\"m0\":%s,\"m1\":%s}", m0 ? "true" : "false", m1 ? "true" : "false");
			char obuf[8];

			print_to_client("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n");
			print_to_client("Content-Length: ");
			snprintf(obuf, 8, "%d\r\n\r\n", (int)strlen(buf));
			print_to_client(obuf);
			print_to_client(buf);
		}
		else if (strcasecmp(tgt, "model.bin") == 0 || strcasecmp(tgt, "model1.bin") == 0) {
			if (reqstate->c.method != HTTP_SERVE_METHOD_GET) goto invmethod;

			FIL f; 
			if (f_open(&f, tgt - 1, FA_READ) == FR_OK) {
				print_to_client("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n");
				stream_file(&f);
				f_close(&f);
			}
			else goto notfound;
		}
		else if (strcasecmp(tgt, "fheap") == 0) {
			if (reqstate->c.method != HTTP_SERVE_METHOD_GET) goto invmethod;

			char buf[16];
			snprintf(buf, 16, "%u", esp_get_free_heap_size());

			print_to_client("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain");
			print_to_client("\r\n\r\n");
			print_to_client(buf);
		}
		else if (strcasecmp(tgt, "newmodel") == 0) {
			if (reqstate->c.method != HTTP_SERVE_METHOD_POST) goto invmethod;

			FIL out_model;
			bool ok = false;
			switch (do_multipart([&](uint8_t * buf, int len, multipart_header_state_t *state){
				if (!state->name_counter) {
					ESP_LOGD(TAG, "no name");
					return false;
				}
				
				if (len == -1) {
					// TODO: error check this
					return !f_open(&out_model, (strcasecmp(state->c.name, "model") == 0) ? "/model.bin" : "/model1.bin", FA_CREATE_ALWAYS | FA_WRITE);
				}
				else if (len == -2) {
					f_close(&out_model);
					ok = true;
					return true;
				}
				else {
					UINT x;
					return !f_write(&out_model, buf, len, &x) && x == len;
				}
			})) {
				case MultipartStatus::EOF_EARLY:
					return;
				case MultipartStatus::INVALID_HEADER:
					send_static_response(400, "Bad Request", "The server was unable to interpret the header area of the form data request.");
					return;
				case MultipartStatus::HOOK_ABORT:
					send_static_response(500, "Internal Server Error", "Unable to write new file");
					return;
				case MultipartStatus::NO_CONTENT_DISP:
					send_static_response(400, "Bad Request", "You are missing a Content-Disposition header in your request.");
					return;
				case MultipartStatus::OK:
					break;
				default:
					return;
			}

			if (!ok) send_static_response(400, "Bad Request", "No files were provided.");
			else send_static_response(204, "No Content", "");
		}
		else if (strcasecmp(tgt, "newui") == 0) {
			if (reqstate->c.method != HTTP_SERVE_METHOD_POST) goto invmethod;

			switch (f_mkdir("0:/upd")) {
				case FR_OK:
				case FR_EXIST:
					break;
				default:
					goto notfound;
			}

			FIL out_ui;
			if (f_open(&out_ui, "/upd/webui.ar", FA_CREATE_ALWAYS | FA_WRITE)) {
				ESP_LOGE(TAG, "Failed to write");
				goto notfound;
			}
			bool ok = false;
			switch (do_multipart([&](uint8_t * buf, int len, multipart_header_state_t *state){
				if (!state->name_counter) {
					ESP_LOGD(TAG, "no name");
					return false;
				}
				
				if (len == -1) {
					return true;
				}
				else if (len == -2) {
					ok = true;
					return true;
				}
				else {
					UINT x;
					return !f_write(&out_ui, buf, len, &x) && x == len;
				}
			})) {
				case MultipartStatus::EOF_EARLY:
				default:
					f_close(&out_ui);
					return;
				case MultipartStatus::INVALID_HEADER:
					send_static_response(400, "Bad Request", "The server was unable to interpret the header area of the form data request.");
					f_close(&out_ui);
					return;
				case MultipartStatus::HOOK_ABORT:
					send_static_response(400, "Bad Request", "You have sent invalid files.");
					f_close(&out_ui);
					return;
				case MultipartStatus::NO_CONTENT_DISP:
					send_static_response(400, "Bad Request", "You are missing a Content-Disposition header in your request.");
					f_close(&out_ui);
					return;
				case MultipartStatus::OK:
					break;
			}

			f_close(&out_ui);

			if (!ok) {
				send_static_response(400, "Bad Request", "Not enough files were provided");
				return;
			}
			
			if (f_open(&out_ui, "/upd/state", FA_CREATE_ALWAYS | FA_WRITE)) {
				send_static_response(500, "Internal Server Error", "Unable to start update");
				return;
			}

			f_putc(0x10, &out_ui);
			f_close(&out_ui);

			send_static_response(200, "OK", "Updating UI.");
			lwip_close(client_sock);
			serial::interface.reset(); // reset both systems
		}
		else if (strcasecmp(tgt, "updatefirm") == 0) {
			if (reqstate->c.method != HTTP_SERVE_METHOD_POST) goto invmethod;

			FIL out_file;
			int gotcount = 0;
			bool ok = false;

			uint16_t stm_csum = 0, esp_csum = 0;

			// Make the update directory
			
			switch (f_mkdir("0:/upd")) {
				case FR_OK:
				case FR_EXIST:
					break;
				default:
					goto notfound;
			}

			// Remove files
			f_unlink("/upd/stm.bin");
			f_unlink("/upd/esp.bin");

			// Temp.
			switch (do_multipart([&](uint8_t * buf, int len, multipart_header_state_t *state){
				if (!state->name_counter) {
					ESP_LOGD(TAG, "no name");
					return false;
				}

				if (len == -1) {
					if (strcasecmp(state->c.name, "stm") == 0) {
						ok = true;
						if (f_open(&out_file, "/upd/stm.bin", FA_WRITE | FA_CREATE_ALWAYS)) {
							ok = false;
							return false;
						}
						ESP_LOGD(TAG, "Writing the stm bin");
						stm_csum = 0;
						return true;
					}
					if (strcasecmp(state->c.name, "esp") == 0) {
						if (f_open(&out_file, "/upd/esp.bin", FA_WRITE | FA_CREATE_ALWAYS)) {
							ok = false;
							return false;
						}
						ESP_LOGD(TAG, "Writing the esp bin");
						esp_csum = 0;
						return true;
					}
					else {
						ESP_LOGD(TAG, "Ignoring");
						return false;
					}
				}
				else if (len == -2) {
					f_close(&out_file);
					++gotcount;
				}
				else {
					// Write the buffer
					UINT bw;
					f_write(&out_file, buf, len, &bw);

					// Update the checksum
					if (strcasecmp(state->c.name, "stm") == 0) 
						stm_csum = util::compute_crc(buf, bw, stm_csum);
					else 												
						esp_csum = util::compute_crc(buf, bw, esp_csum);
				}
				return true;
			})) {
				case MultipartStatus::EOF_EARLY:
					return;
				case MultipartStatus::INVALID_HEADER:
					send_static_response(400, "Bad Request", "The server was unable to interpret the header area of the form data request.");
					return;
				case MultipartStatus::HOOK_ABORT:
					send_static_response(400, "Bad Request", "You have sent invalid files.");
					return;
				case MultipartStatus::NO_CONTENT_DISP:
					send_static_response(400, "Bad Request", "You are missing a Content-Disposition header in your request.");
					return;
				case MultipartStatus::OK:
					break;
				default:
					return;
			}

			if (gotcount < 1 || !ok) {
				send_static_response(400, "Bad Request", "Not enough files were provided");
				return;
			}

			// Alright we've got stuff. Write the csum file now.
			f_open(&out_file, "/upd/chck.sum", FA_WRITE | FA_CREATE_ALWAYS);
			UINT bw;
			f_write(&out_file, &esp_csum, 2, &bw);
			f_write(&out_file, &stm_csum, 2, &bw);
			f_close(&out_file);

			// Set the update state for update
			f_open(&out_file, "/upd/state", FA_WRITE | FA_CREATE_ALWAYS);
			f_putc(0, &out_file);
			f_close(&out_file);

			send_static_response(200, "OK", gotcount == 2 ? "Updating stm+esp" : "Updating stm only");
			lwip_close(client_sock);
			serial::interface.reset();
		}
		else if (strcasecmp(tgt, "reboot") == 0) {
			// Just reboot
			send_static_response(204, "No Content", "");
			lwip_close(client_sock);
			serial::interface.reset();
		}
		else {
			// Temp.
			send_static_response(404, "Not Found", "Api method not recognized.");
		}
		return;
invmethod:
		send_static_response(405, "Method Not Allowed", "Invalid API method.");
		return;
badrequest:
		send_static_response(400, "Bad Request", "Invalid parameters to API method.");
		return;
notfound:
		send_static_response(404, "Not Found", "The data that resource points to does not exist.");
		return;
	}

	// Send this file as a response
	void send_cacheable_file(const char * filename) {
		char real_filename[32];

		if (reqstate->c.is_gzipable) {
			snprintf(real_filename, 32, "web/%s.gz", filename);
			if (f_stat(real_filename, NULL)) {
				ESP_LOGE(TAG, "Missing gzipped file from SD web archive... aborting (%s)", real_filename);
				reqstate->c.is_gzipable = false;
			}
		}
		if (!reqstate->c.is_gzipable) {
			snprintf(real_filename, 32, "web/%s", filename);
			if (f_stat(real_filename, NULL)) {
				ESP_LOGE(TAG, "Missing key file from SD web archive... aborting (%s)", real_filename);
				return;
			}
		}

		FIL f; f_open(&f, real_filename, FA_READ);
		if (reqstate->c.is_gzipable) print_to_client("Content-Encoding: gzip\r\n");
		stream_file(&f);
		f_close(&f);
	}

	void start_real_response() {
		// First we verify the authentication
		switch (reqstate->c.auth_type) {
			case HTTP_SERVE_AUTH_TYPE_NO_AUTH:
				send_static_response(401, "Unauthorized", "No authentication was provided", "WWW-Authenticate: Basic realm=\"msign\"");
				return;
			case HTTP_SERVE_AUTH_TYPE_INVALID_TYPE:
				send_static_response(401, "Unauthorized", "Invalid authentication type.");
				return;
			case HTTP_SERVE_AUTH_TYPE_OK:
				if (!check_auth(reqstate->c.auth_string)) {
					send_static_response(403, "Forbidden", "Invalid authentication.");
					return;
				}
				break;
		}

		// Check if this is an API method
		if (strncmp(reqstate->c.url, "a/", 2) == 0) {
			do_api_response(reqstate->c.url + 2);
		}
		else {
			// Only allow GET requests
			if (reqstate->c.method != HTTP_SERVE_METHOD_GET) {
				send_static_response(405, "Method Not Allowed", "Static resources are only gettable with GET");
				return;
			}
			// Check for a favicon
			if (strcasecmp(reqstate->c.url, "favicon.ico") == 0) {
				ESP_LOGD(TAG, "Sending 404");
				// Send a 404
				send_static_response(404, "Not Found", "No favicon is present");
				return;
			}

			char valid_etag[16];
			const char * actual_name = nullptr;
			const char * content_type = nullptr;

			// Check for the js files
			if (strcasecmp(reqstate->c.url, "page.js") == 0) {
				snprintf(valid_etag, 16, "E%djs", etag_num);
				actual_name = "page.js";
				content_type = "application/javascript";
			}
			else if (strcasecmp(reqstate->c.url, "page.css") == 0) {
				snprintf(valid_etag, 16, "E%dcss", etag_num);
				actual_name = "page.css";
				content_type = "text/css";
			}
			else {
				snprintf(valid_etag, 16, "E%dht", etag_num);
				actual_name = "page.html";
				content_type = "text/html; charset=UTF-8";
			}

			// Check for etag-ability
			if (reqstate->c.is_conditional && strcmp(valid_etag, reqstate->c.etag) == 0) {
				// Send a 304
				print_to_client("HTTP/1.1 304 Not Modified\r\nCache-Control: max-age=172800, stale-while-revalidate=604800\r\nConnection: close\r\nETag: ");
				print_to_client(valid_etag);
				print_to_client("\r\n\r\n");
			}
			else {
				ESP_LOGD(TAG, "Starting response");
				// Start a response
				print_to_client("HTTP/1.1 200 OK\r\nCache-Control: max-age=172800, stale-while-revalidate=604800\r\nConnection: close\r\nContent-Type: ");

				// Send the content type
				print_to_client(content_type);
				// Send the etag
				print_to_client("\r\nETag: \"");
				print_to_client(valid_etag);
				print_to_client("\"\r\n");

				// Send the file contents
				send_cacheable_file(actual_name);
			}
		}
	}

	void start_response() {
		ESP_LOGD(TAG, "Got request from server");
		switch (reqstate->c.error_code) {
			case HTTP_SERVE_ERROR_CODE_OK:
				start_real_response();
				break;
			case HTTP_SERVE_ERROR_CODE_BAD_METHOD:
				send_static_response(501, "Not Implemented", "This server only implements GET and POST requests");
				break;
			case HTTP_SERVE_ERROR_CODE_UNSUPPORTED_VERSION:
				send_static_response(505, "HTTP Version Not Supported", "This server only supports versions 1.0 and 1.1");
				break;
			case HTTP_SERVE_ERROR_CODE_URL_TOO_LONG:
				// Send a boring old request
				send_static_response(414, "URI Too Long", "The URI specified was too long.");
				break;
			case HTTP_SERVE_ERROR_CODE_BAD_REQUEST:
				// Send a boring old request
				send_static_response(400, "Bad Request", "You have sent an invalid request");
				break;
			default:
				// Send a boring old request
				send_static_response(500, "Internal Server Error", "The server was unable to interpret your request for an unspecified reason.");
				break;
		}
		lwip_close(client_sock);
		client_sock = -1;
	}

	void run(void*) {
		if (upd::needed() == upd::WEB_UI) {
			ESP_LOGI(TAG, "Running webui update");
			upd::update_website();
		}

		ESP_LOGI(TAG, "Starting webui");
		xEventGroupWaitBits(wifi::events, wifi::WifiConnected, 0, true, portMAX_DELAY);
		// check if an etag file exists on disk (deleted during updates)
		if (f_stat("/web/etag", NULL)) {
			esp_fill_random(&etag_num, 4);

			FIL fl; f_open(&fl, "/web/etag", FA_CREATE_ALWAYS | FA_WRITE);
			UINT x;
			f_write(&fl, &etag_num, 4, &x);
			f_close(&fl);
		}
		else {
			FIL fl; f_open(&fl, "/web/etag", FA_READ);
			UINT x;
			f_read(&fl, &etag_num, 4, &x);
			f_close(&fl);
		}
		
		// Open the socket
		client_sock = -1;
		server_sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
		if (server_sock < 0) {
			ESP_LOGE(TAG, "unable to create socket");
			while (1) {vTaskDelay(1000);}
		}

		{
			sockaddr_in baddr{};
			baddr.sin_family = AF_INET;
			baddr.sin_addr.s_addr = INADDR_ANY;
			baddr.sin_port = lwip_htons(80);
			bzero(baddr.sin_zero, sizeof(baddr.sin_zero));

			if (lwip_bind(server_sock, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
				lwip_close(server_sock);
				ESP_LOGE(TAG, "Failed to bind socket.");
				while (1) {vTaskDelay(1000);}
			}

		}

		lwip_listen(server_sock, 2);
		reqstate = nullptr;
		ESP_LOGI(TAG, "Started webui");

		while (true) {
			if (client_sock >= 0) {
				// Deal with the client
				while (client_sock >= 0) {
					uint8_t x;
					if (read_from_client(&x, 1) != 1) {
						ESP_LOGW(TAG, "Failed to read req");
						lwip_close(client_sock);
						client_sock = -1;
						break;
					}
					switch (http_serve_feed(x, false, reqstate)) {
						case HTTP_SERVE_FAIL:
							reqstate->c.error_code = HTTP_SERVE_ERROR_CODE_BAD_REQUEST;
							[[fallthrough]];
						case HTTP_SERVE_DONE:
							// Done handling
							start_response();
						default:
							break;
					}
				}
			}
			else {
				if (reqstate) {
					delete reqstate;
					reqstate = nullptr;
				}
				client_sock = lwip_accept(server_sock, NULL, NULL);
				if (client_sock >= 0) {
					reqstate = new http_serve_state_t;
					http_serve_start(reqstate);
				}
				else client_sock = -1;
			}
		}
	}
}
