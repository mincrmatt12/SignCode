#include "ttc.h"
#include "json.h"
#include "oauth1.h"
#include "serial.h"
#include "time.h"
#include "TimeLib.h"
#include "wifi.h"
#include <inttypes.h>
#include "config.h"
#include "string.h"
#include "util.h"

slots::TTCInfo ttc::info;
slots::TTCTime ttc::times[6];

uint64_t ttc::time_since_last_update = 0;
uint64_t      time_since_last_alert = 0;
char * ttc::alert_buffer = nullptr;

void ttc::init() {
	memset(ttc::times, 0, sizeof(ttc::times));
    ttc::info = { 0 };

	serial::interface.register_handler(&ttc::on_open);
}

void ttc::loop() {
	if ((now() - time_since_last_update > 15 || time_since_last_update == 0) && wifi::available()) {
		// update the ttc times
		ttc::info.flags = 0;
		for (uint8_t slot = 0; slot < 3; ++slot) {
			const char * stopid = config::manager.get_value((config::Entry)(config::STOPID1 + slot));
			if (stopid != nullptr) {
				const char * dtag = config::manager.get_value((config::Entry)(config::DTAG1 + slot));
				const char * name = config::manager.get_value((config::Entry)(config::SNAME1 + slot));
				serial::interface.update_data(slots::TTC_NAME_1 + slot, (const uint8_t *)name, strlen(name));
				ttc::info.nameLen[slot] = strlen(name);

				do_update(stopid, dtag, slot);
			}
		}

		serial::interface.update_data(slots::TTC_INFO, (const uint8_t *)&info, sizeof(info));
		time_since_last_update = now();
	}
	if ((now() - time_since_last_alert > 60 || time_since_last_alert == 0) && wifi::available()) {
		// update the alerts
		do_alert_update();
		time_since_last_alert = now();
	}
}

void ttc::on_open(uint16_t data_id) {
	Log.println("onopen: ");
	Log.println(data_id);
	switch (data_id) {
		case slots::TTC_INFO:
			serial::interface.update_data(slots::TTC_INFO, (const uint8_t *)&info, sizeof(info));
			break;
		case slots::TTC_TIME_1:
		case slots::TTC_TIME_2:
		case slots::TTC_TIME_3:
			serial::interface.update_data(data_id, (const uint8_t *)(&times[data_id - slots::TTC_TIME_1]), sizeof(slots::TTCTime));
			break;
		case slots::TTC_TIME_1B:
		case slots::TTC_TIME_2B:
		case slots::TTC_TIME_3B:
			serial::interface.update_data(data_id, (const uint8_t *)(&times[3 + data_id - slots::TTC_TIME_1B]), sizeof(slots::TTCTime));
			break;
		case slots::TTC_NAME_1:
		case slots::TTC_NAME_2:
		case slots::TTC_NAME_3:
			{
				const char * name = config::manager.get_value((config::Entry)(config::SNAME1 + (data_id - slots::TTC_NAME_1)));
				if (name == nullptr) return;
				serial::interface.update_data(data_id, (const uint8_t *)name, strlen(name));
				break;
			}
	}
}

void ttc::do_update(const char * stop, const char * dtag, uint8_t slot) {
	char url[80];
	snprintf_P(url, 80, PSTR("/service/publicJSONFeed?command=predictions&a=ttc&stopId=%s"), stop);

	Log.println(url);
	int16_t status_code;
	auto cb = util::download_with_callback("webservices.nextbus.com", url, status_code);

	if (status_code < 200 || status_code > 299) {
		util::stop_download();
		return;
	}

	// message is here now read it
	
	struct State {
		uint8_t e1;
		uint8_t e2;

		uint64_t epoch = 0;
		bool layover = false;
		bool tag = false;
	} state;

	char * dirtag = strdup(dtag);
	
	json::JSONParser parser([&](json::PathNode ** stack, uint8_t stack_ptr, const json::Value& v){
		if (stack_ptr < 2) return;

		json::PathNode &top = *stack[stack_ptr-1];
		json::PathNode &parent = *stack[stack_ptr-2];

		if ((parent.is_array() || parent.is_obj()) && strcmp_P(parent.name, PSTR("prediction")) == 0) {
			// use the first two only

			state.e1 = stack[1]->index;
			state.e2 = parent.index;
			
			if (strcmp_P(top.name, PSTR("affectedByLayover")) == 0) {
				// is it's value true?
				if (v.type == json::Value::STR && strcmp_P(v.str_val, PSTR("true")) == 0) {
					// mark as delayed
					state.layover = true;
				}
				else {
					state.layover = false;
				}
			}
			if (strcmp_P(top.name, PSTR("dirTag")) == 0 && v.type == json::Value::STR)
			{
				strcpy(dirtag, dtag);
				char *test_str = strtok(dirtag, ",");
				while (test_str != NULL) {
					Log.println(test_str);
					Log.println(v.str_val);
					Log.println(parent.is_array() ? parent.index : -1);
					if (strcmp(test_str, v.str_val) == 0) {
						state.tag = true; break;
					}
					else {
						test_str = strtok(NULL, ",");
					}
				}
			}
			if (strcmp_P(top.name, PSTR("epochTime")) == 0 && v.type == json::Value::STR) {
				state.epoch = signtime::millis_to_local(atoll(v.str_val));
			}
		}
		else if (top.is_array() && strcmp_P(top.name, PSTR("prediction")) == 0 && v.type == json::Value::OBJ) {
			if (state.tag && state.e2 < 4) {
				ttc::info.flags |= (slots::TTCInfo::EXIST_0 << slot);
				if (state.epoch < ttc::times[slot].tA || ttc::times[slot].tA == 0) {
					ttc::times[slot+3].tB = ttc::times[slot+3].tA;
					ttc::times[slot+3].tA = ttc::times[slot].tB;
					ttc::times[slot].tB = ttc::times[slot].tA;
					ttc::times[slot].tA = state.epoch;
					if (state.layover) {
						ttc::info.flags |= (slots::TTCInfo::DELAY_0 << slot);
					}
				}
				else if (state.epoch < ttc::times[slot].tB || ttc::times[slot].tB == 0) {
					ttc::times[slot+3].tB = ttc::times[slot+3].tA;
					ttc::times[slot+3].tA = ttc::times[slot].tB;
					ttc::times[slot].tB = state.epoch;
					if (state.layover) {
						ttc::info.flags |= (slots::TTCInfo::DELAY_0 << slot);
					}
				}
				else if (state.epoch < ttc::times[slot+3].tA || ttc::times[slot+3].tA == 0) {
					ttc::times[slot+3].tB = ttc::times[slot+3].tA;
					ttc::times[slot+3].tA = state.epoch;
					if (state.layover) {
						ttc::info.flags |= (slots::TTCInfo::DELAY_0 << slot);
					}
				}
				else if (state.epoch < ttc::times[slot+3].tB || ttc::times[slot+3].tB == 0) {
					ttc::times[slot+3].tB = state.epoch;
					if (state.layover) {
						ttc::info.flags |= (slots::TTCInfo::DELAY_0 << slot);
					}
				}

				Log.print(F("Adding ttc entry in slot "));
				Log.print(slot);
			}
			state.tag = false;
			state.layover = false;
			state.epoch = 0;
		}
	});

	auto bkp1 = ttc::times[slot], bkp2 = ttc::times[slot + 3];

	memset(&ttc::times[slot], 0, sizeof(ttc::times[0]));
	memset(&ttc::times[slot+3], 0, sizeof(ttc::times[0]));

	if (!parser.parse(std::move(cb))) {
		Log.println(F("JSON fucked up."));

		ttc::times[slot] = bkp1;
		ttc::times[slot + 3] = bkp2;
	} // parse while calling our function.

	on_open(slots::TTC_TIME_1 + slot);
	on_open(slots::TTC_TIME_1B + slot);

	util::stop_download();
	free(dirtag);
}

void ttc::do_alert_update() {
	// Really shitty algorithm.
	//
	// Take all the tweets that match.
	// If they match the "regular service" pattern, mark any associated tweets as invalid. Otherwise, push them to the buffer.
	
	char *saved_texts[6] = {0};
	int saved_index = 0;
	char url[160] = {0};

	bool alert_flags[4] = {false}; // SUBWAY is at address 3

	// Do the twitter thingie
	
	if (config::manager.get_value(config::Entry::ALERT_SEARCH) != nullptr) {
		char query[50] = {0};
		char query_unencoded[40] = {0};
		snprintf_P(query_unencoded, 40, PSTR("from:TTCnotices \"%s\""), config::manager.get_value(config::Entry::ALERT_SEARCH));
		oauth1::percent_encode(query_unencoded, query);
		snprintf_P(url, 160, PSTR("/1.1/search/tweets.json?result_type=recent&count=10&q=%s"), query);

		const char *params[][2] = {
			{"count", "10"},
			{"q", query_unencoded},
			{"result_type", "recent"},
			{nullptr, nullptr}
		};

		char * authorization = oauth1::generate_authorization(params, "https://api.twitter.com/1.1/search/tweets.json");

		// Send request
		int16_t sco;

		const char *headers[][2] = {
			{"Authorization", authorization},
			{nullptr, nullptr}
		};

		auto cb = util::download_with_callback("_api.twitter.com", url, headers, sco);
		free(authorization); // The headers are already sent at this point.

		if (sco != 200) {
			Log.println(F("asdf12failpoopoo"));
			util::stop_download();
			return;
		}

		int64_t superseded[10] = {0};
		int sindex = 0;
		
		int64_t current = 0;
		auto parser = json::JSONParser([&](json::PathNode ** stack, uint8_t stack_ptr, const json::Value& v){
			if (stack_ptr < 2) return;
			// Check if we are in the zone

			json::PathNode &overall = *stack[1];
			json::PathNode &parent = *stack[stack_ptr - 2];
			json::PathNode &top = *stack[stack_ptr - 1];

			if (strcmp_P(overall.name, PSTR("statuses")) || !overall.is_array()) return;

			// We are in a result.
			if (strcmp_P(top.name, PSTR("id")) == 0 && stack_ptr == 3 && v.type == json::Value::INT) {
				// Get ID
				current = v.int_val;
				return;
			}

			if (strcmp_P(top.name, PSTR("text")) == 0 && stack_ptr == 3 && v.type == json::Value::STR) {
				for (int i = 0; i < sindex; ++i) {
					if (superseded[i] == current) return;
				}
				// We have the text. Filter it to find the actual status
				Log.println(F("Got tweet:"));
				Log.println(v.str_val);
				
				const char *begin = v.str_val; int size = 0;
				while (*begin != ':') {
					if (*begin == 0) {Log.println(F("pfail1")); return;}
					++begin;
				}
				if (*++begin != ' ') {Log.println(F("pfail2")); return;}
				++begin;

				while (begin[size] != '.') {
					if (begin[size] == 0) {Log.println(F("pfail3")); return;}
					++size;
				}

				if (size < 6) return;

				char * real = (char *)malloc(size+1);
				strncpy(real, begin, size);
				real[size] = 0;

				Log.println(F("Got actual:"));
				Log.println(real);
				
				// Is this a regular service

				if (strstr_P(real, PSTR("Regular service has "))) {
					// Yes, so we don't care that much
					free(real);
					return;
				}

				// Otherwise, we should record this.
				saved_texts[saved_index++] = real;
				alert_flags[3] = true;
				return;
			}

			// Are we superseded something?
			if ((strcmp_P(top.name, PSTR("in_reply_to_status_id")) == 0 && stack_ptr == 3 && v.type == json::Value::INT) ||
				(strcmp_P(top.name, PSTR("id")) == 0 && stack_ptr == 4 && strcmp_P(parent.name, PSTR("quoted_status")) == 0 && v.type == json::Value::INT)) {
				Log.print(F("superseding:"));
				Log.println((int)v.int_val);
				superseded[sindex++] = v.int_val;
			}
		});

		if (!parser.parse(std::move(cb))) {
			Log.println(F("oh no"));
		}
	}

	// Generate an alert string.
	
	size_t length = saved_index ? (saved_index - 1) * 3 : 0;
	for (int i = 0; i < saved_index; ++i) {
		length += strlen(saved_texts[i]);
	}

	alert_buffer = (char *)realloc(alert_buffer, length + 1);
	alert_buffer[length] = 0;

	char *ptr = alert_buffer;

	for (int i = 0; i < saved_index; ++i) {
		if (i == saved_index - 1) {
			ptr += sprintf_P(ptr, "%s", saved_texts[i]);
		}
		else {
			ptr += sprintf_P(ptr, "%s / ", saved_texts[i]);
		}
	}

	Log.println(F("Alert str:"));
	Log.println(alert_buffer);
}
