#include <Arduino.h>
#pragma region OTA INCLUDES
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Ticker.h>
#include "html.h"
#define SSID_FORMAT "ESP32-%06lX"
#define PASSWORD "test123456"
#pragma endregion
#include "Arduino_Helpers.h"
#include "AH/Timing/MillisMicrosTimer.hpp"
#include "esPod.h"
#ifdef ENABLE_A2DP
	#include "AudioTools.h"
	#include "BluetoothA2DPSink.h"
		#ifdef USE_EXTERNAL_DAC_UDA1334A
			I2SStream i2s;
			BluetoothA2DPSink a2dp_sink(i2s);
		#endif
		#ifdef USE_INTERNAL_DAC
			AnalogAudioStream out;
			BluetoothA2DPSink a2dp_sink(out);
		#endif
		#ifdef AUDIOKIT
			#ifndef LED_BUILTIN
				#define LED_BUILTIN 22
			#else
				#undef LED_BUILTIN
				#define LED_BUILTIN 22
			#endif
			#include "AudioTools/AudioLibs/I2SCodecStream.h"
			#include "AudioBoard.h"
			AudioInfo info(44100,2,16);
			I2SCodecStream i2s(AudioKitEs8388V1);
			BluetoothA2DPSink a2dp_sink(i2s);
		#endif
#endif

#ifndef AUDIOKIT
	esPod espod(Serial2);
#else
	//HardwareSerial ipodSerial(1);
	//esPod espod(ipodSerial);
	esPod espod(Serial);
	#ifdef SERIAL_DEBUG
		#undef SERIAL_DEBUG
	#endif
#endif
#ifndef REFRESH_INTERVAL
	#define REFRESH_INTERVAL 5
#endif
Timer<millis> espodRefreshTimer = REFRESH_INTERVAL;
Timer<millis> OTATimer = 150;

char incAlbumName[255] 		= 	"incAlbum";
char incArtistName[255] 	= 	"incArtist";
char incTrackTitle[255] 	= 	"incTitle";
uint32_t incTrackDuration 	= 	0;
bool albumNameUpdated 		= 	false;
bool artistNameUpdated 		= 	false;
bool trackTitleUpdated 		= 	false;
bool trackDurationUpdated		=	false;

#ifdef ENABLE_A2DP
/// @brief callback on changes of A2DP connection and AVRCP connection. Turns a LED on, enables the espod.
/// @param state New state passed by the callback.
/// @param ptr Not used.
void connectionStateChanged(esp_a2d_connection_state_t state, void* ptr) {
	switch (state)	{
		case ESP_A2D_CONNECTION_STATE_CONNECTED:
			#ifdef LED_BUILTIN
				digitalWrite(LED_BUILTIN,!digitalRead(LED_BUILTIN));
			#endif
			#ifdef DEBUG_MODE
				Serial.println("ESP_A2D_CONNECTION_STATE_CONNECTED, espod enabled");
			#endif
			espod.disabled = false;
			break;
		case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
			#ifdef LED_BUILTIN
				digitalWrite(LED_BUILTIN,!digitalRead(LED_BUILTIN));
			#endif
			#ifdef DEBUG_MODE
				Serial.println("ESP_A2D_CONNECTION_STATE_DISCONNECTED, espod disabled");
			#endif
			espod.resetState();
			espod.disabled = true;
			break;
	}
}

/// @brief Callback for the change of playstate after connection. Aligns the state of the esPod to the state of the phone. Play should be called by the espod interaction
/// @param state The A2DP Stream to align to.
/// @param ptr Not used.
void audioStateChanged(esp_a2d_audio_state_t state,void* ptr) {
	switch (state)	{
		case ESP_A2D_AUDIO_STATE_STARTED:
			espod.playStatus = PB_STATE_PLAYING;
			#ifdef DEBUG_MODE
				Serial.println("ESP_A2D_AUDIO_STATE_STARTED, espod.playStatus = PB_STATE_PLAYING");
			#endif
			break;
		case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
			espod.playStatus = PB_STATE_PAUSED;
			#ifdef DEBUG_MODE
				Serial.println("ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND, espod.playStatus = PB_STATE_PAUSED");
			#endif
			break;
		case ESP_A2D_AUDIO_STATE_STOPPED:
			espod.playStatus = PB_STATE_STOPPED;
			#ifdef DEBUG_MODE
				Serial.println("ESP_A2D_AUDIO_STATE_STOPPED, espod.playStatus = PB_STATE_STOPPED");
			#endif
			break;
	}
}

/// @brief Play position callback returning the ms spent since start on every interval
/// @param play_pos Playing Position in ms
void avrc_rn_play_pos_callback(uint32_t play_pos) {
	espod.playPosition = play_pos;
	if(espod.playStatusNotificationState==NOTIF_ON && espod.trackChangeAckPending==0x00) {
		espod.L0x04_0x27_PlayStatusNotification(0x04,play_pos);
	}
}

/// @brief Catch callback for the AVRC metadata. There can be duplicates !
/// @param id Metadata attribute ID : ESP_AVRC_MD_ATTR_xxx
/// @param text Text data passed around, sometimes it's a uint32_t
void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
	switch (id)	{

		case ESP_AVRC_MD_ATTR_ALBUM:
			strcpy(incAlbumName,(char*)text); //Buffer the incoming album string
			if(espod.trackChangeAckPending>0x00) { //There is a pending metadata update
				if(!albumNameUpdated) { //The album Name has not been updated yet
					strcpy(espod.albumName,incAlbumName);
					albumNameUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Album rxed, ACK pending, albumNameUpdated to %s \n",espod.albumName);
					#endif
				}
				else {
					#ifdef DEBUG_MODE
						Serial.printf("Album rxed, ACK pending, already updated to %s \n",espod.albumName);
					#endif
				}
			}
			else { //There is no pending track change from iPod : active or passive track change from avrc target
				if(strcmp(incAlbumName,espod.albumName)!=0) { //Different incoming metadata
					strcpy(espod.prevAlbumName,espod.albumName);
					strcpy(espod.albumName,incAlbumName);
					albumNameUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Album rxed, NO ACK pending, albumNameUpdated to %s \n",espod.albumName);
					#endif
				}
				else { //Despammer for double sends
					#ifdef DEBUG_MODE
						Serial.printf("Album rxed, NO ACK pending, already updated to %s \n",espod.albumName);
					#endif
				}
			}
			break;


		case ESP_AVRC_MD_ATTR_ARTIST:
			strcpy(incArtistName,(char*)text); //Buffer the incoming artist string
			if(espod.trackChangeAckPending>0x00) { //There is a pending metadata update
				if(!artistNameUpdated) { //The artist name has not been updated yet
					strcpy(espod.artistName,incArtistName);
					artistNameUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Artist rxed, ACK pending, artistNameUpdated to %s \n",espod.artistName);
					#endif
				}
				else {
					#ifdef DEBUG_MODE
						Serial.printf("Artist rxed, ACK pending, already updated to %s \n",espod.artistName);
					#endif
				}
			}
			else { //There is no pending track change from iPod : active or passive track change from avrc target
				if(strcmp(incArtistName,espod.artistName)!=0) { //Different incoming metadata
					strcpy(espod.prevArtistName,espod.artistName);
					strcpy(espod.artistName,incArtistName);
					artistNameUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Artist rxed, NO ACK pending, artistNameUdpated to %s \n",espod.artistName);
					#endif
				}
				else { //Despammer for double sends
					#ifdef DEBUG_MODE
						Serial.printf("Artist rxed, NO ACK pending, already updated to %s \n",espod.artistName);
					#endif
				}
			}
			break;


		case ESP_AVRC_MD_ATTR_TITLE: //Title change triggers the NEXT track assumption if unexpected. It is too intensive to try to do NEXT/PREV guesswork
			strcpy(incTrackTitle,(char*)text); //Buffer the incoming track title
			if(espod.trackChangeAckPending>0x00) { //There is a pending metadata update
				if(!trackTitleUpdated) { //The track title has not been updated yet
					strcpy(espod.trackTitle,incTrackTitle);
					trackTitleUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Title rxed, ACK pending, trackTitleUpdated to %s\n",espod.trackTitle);
					#endif
				}
				else {
					#ifdef DEBUG_MODE
						Serial.printf("Title rxed, ACK pending, already updated to %s \n",espod.trackTitle);
					#endif
				}
			}
			else { //There is no pending track change from iPod : active or passive track change from avrc target
				if(strcmp(incTrackTitle,espod.trackTitle)!=0) { //Different from current track Title -> Systematic NEXT
					//Assume it is Next, perform cursor operations
					espod.trackListPosition = (espod.trackListPosition + 1 ) % TOTAL_NUM_TRACKS;
					espod.prevTrackIndex = espod.currentTrackIndex;
					espod.currentTrackIndex = (espod.currentTrackIndex + 1 ) % TOTAL_NUM_TRACKS;
					espod.trackList[espod.trackListPosition] = (espod.currentTrackIndex);
					//Copy new title and flag that data has been updated
					strcpy(espod.prevTrackTitle,espod.trackTitle);
					strcpy(espod.trackTitle,incTrackTitle);
					trackTitleUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Title rxed, NO ACK pending, AUTONEXT, trackTitleUpdated to %s\n",espod.trackTitle);
						Serial.printf("\ttrackPos %d trackIndex %d\n",espod.trackListPosition,espod.currentTrackIndex);
					#endif
				}
				else { //Despammer for double sends
					#ifdef DEBUG_MODE
						Serial.printf("Title rxed, NO ACK pending, same name : %s \n",espod.trackTitle);
					#endif
				}
			}
			break;


		case ESP_AVRC_MD_ATTR_PLAYING_TIME: 
			incTrackDuration = String((char*)text).toInt();
			if(espod.trackChangeAckPending>0x00) { //There is a pending metadata update
				if(!trackDurationUpdated) { //The duration has not been updated yet
					espod.trackDuration = incTrackDuration;
					trackDurationUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Duration rxed, ACK pending, trackDurationUpdated to %d \n",espod.trackDuration);
					#endif
				}
				else {
					#ifdef DEBUG_MODE
						Serial.printf("Duration rxed, ACK pending, already updated to %d \n",espod.trackDuration);
					#endif
				}
			}
			else { //There is no pending track change from iPod : active or passive track change from avrc target
				if(incTrackDuration != espod.trackDuration) { //Different incoming metadata
					espod.trackDuration = incTrackDuration;
					trackDurationUpdated = true;
					#ifdef DEBUG_MODE
						Serial.printf("Duration rxed, NO ACK pending, trackDurationUpdated to %d \n",espod.trackDuration);
					#endif
				}
				else { //Despammer for double sends
					#ifdef DEBUG_MODE
						Serial.printf("Duration rxed, NO ACK pending, already updated to %d \n",espod.trackDuration);
					#endif
				}
			}
			break;
	}



	//Check if it is tie to send a notification
	if(albumNameUpdated && artistNameUpdated && trackTitleUpdated && trackDurationUpdated ) { 
		//If all fields have received at least one update and the trackChangeAckPending is still hanging. The failsafe for this one is in the espod.refresh()
		if (espod.trackChangeAckPending>0x00) {
			#ifdef DEBUG_MODE
				Serial.printf("Artist+Album+Title+Duration +++ ACK Pending 0x%x\n",espod.trackChangeAckPending);
				Serial.printf("\tPending duration: %d\n",millis()-espod.trackChangeTimestamp);
			#endif
			espod.L0x04_0x01_iPodAck(iPodAck_OK,espod.trackChangeAckPending);
			espod.trackChangeAckPending = 0x00;
			#ifdef DEBUG_MODE
				Serial.println("trackChangeAckPending reset to 0x00");
			#endif 
		}
		albumNameUpdated 	= 	false;
		artistNameUpdated 	= 	false;
		trackTitleUpdated 	= 	false;
		trackDurationUpdated=	false;
		#ifdef DEBUG_MODE
			Serial.println("Artist+Album+Title+Duration true -> False");
		#endif
		//Inform the car
		if (espod.playStatusNotificationState==NOTIF_ON) {
			espod.L0x04_0x27_PlayStatusNotification(0x01,espod.currentTrackIndex);
		}
	}
}

#endif

/// @brief Callback function that passes intended operations from the esPod to the A2DP player
/// @param playCommand A2DP_xx command instruction. It does not match the PB_CMD_xx codes !!!
void playStatusHandler(byte playCommand) {
	#ifdef ENABLE_A2DP
  	switch (playCommand) {
		case A2DP_STOP:
			a2dp_sink.stop();
			#ifdef DEBUG_MODE
				Serial.println("playStatusHandler: A2DP_STOP");
			#endif
			//Stoppage notification is handled internally in the espod
			break;
		case A2DP_PLAY:
			a2dp_sink.play();
			#ifdef DEBUG_MODE
				Serial.println("playStatusHandler: A2DP_PLAY");
			#endif
			//Watch out for possible metadata
			break;
		case A2DP_PAUSE:
			a2dp_sink.pause();
			#ifdef DEBUG_MODE
				Serial.println("playStatusHandler: A2DP_PAUSE");
			#endif
			break;
		case A2DP_REWIND:
			a2dp_sink.previous();
			#ifdef DEBUG_MODE
				Serial.println("playStatusHandler: A2DP_REWIND");
			#endif
			break;
		case A2DP_NEXT:
			a2dp_sink.next();
			#ifdef DEBUG_MODE
				Serial.println("playStatusHandler: A2DP_NEXT");
			#endif
			break;
		case A2DP_PREV: 
			a2dp_sink.previous();
			#ifdef DEBUG_MODE
				Serial.println("playStatusHandler: A2DP_PREV");
			#endif
			break;
	}
  	#endif
}

#pragma region OTA
WebServer server(80);
Ticker tkSecond;
uint8_t otaDone = 0;

const char *alphanum = "0123456789!@#$%^&*abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
String generatePass(uint8_t str_len) {
  String buff;
  for (int i = 0; i < str_len; i++) {
    buff += alphanum[random(strlen(alphanum) - 1)];
  }
  return buff;
}

void apMode() {
  char ssid[13];
  char passwd[11];
  long unsigned int espmac = ESP.getEfuseMac() >> 24;
  snprintf(ssid, 13, SSID_FORMAT, espmac);
#ifdef PASSWORD
  snprintf(passwd, 11, PASSWORD);
#else
  snprintf(passwd, 11, generatePass(10).c_str());
#endif
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, passwd);  // Set up the SoftAP
  MDNS.begin("esp32");
  Serial.printf("AP: %s, PASS: %s\n", ssid, passwd);
}

void handleUpdateEnd() {
  server.sendHeader("Connection", "close");
  if (Update.hasError()) {
    server.send(502, "text/plain", Update.errorString());
  } else {
    server.sendHeader("Refresh", "10");
    server.sendHeader("Location", "/");
    server.send(307);
    ESP.restart();
  }
}

void handleUpdate() {
  size_t fsize = UPDATE_SIZE_UNKNOWN;
  if (server.hasArg("size")) {
    fsize = server.arg("size").toInt();
  }
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Receiving Update: %s, Size: %d\n", upload.filename.c_str(), fsize);
    if (!Update.begin(fsize)) {
      otaDone = 0;
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    } else {
      otaDone = 100 * Update.progress() / Update.size();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
    } else {
      Serial.printf("%s\n", Update.errorString());
      otaDone = 0;
    }
  }
}

void webServerInit() {
  server.on(
    "/update", HTTP_POST,
    []() {
      handleUpdateEnd();
    },
    []() {
      handleUpdate();
    }
  );
  server.on("/favicon.ico", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "image/x-icon", favicon_ico_gz, favicon_ico_gz_len);
  });
  server.onNotFound([]() {
    server.send(200, "text/html", indexHtml);
  });
  server.begin();
  Serial.printf("Web Server ready at http://esp32.local or http://%s\n", WiFi.softAPIP().toString().c_str());
}

void everySecond() {
  if (otaDone > 1) {
    Serial.printf("ota: %d%%\n", otaDone);
  }
}

#pragma endregion

void setup() {
	#pragma region OTA INTERCEPT
	pinMode(5,INPUT);
	bool stopForOTA = false;
	unsigned long debounce = millis();
	while(!digitalRead(5) && !stopForOTA) {
		if(millis()-debounce > 500) {
			stopForOTA = true;
			Serial.begin(115200);
			apMode();
			webServerInit();
			tkSecond.attach(1, everySecond);
		}
	}
	while(stopForOTA) {
			if(OTATimer) {
			server.handleClient();
		}
	}
	#pragma endregion

	#ifdef ENABLE_A2DP
		#ifdef USE_EXTERNAL_DAC_UDA1334A
			auto cfg = i2s.defaultConfig(TX_MODE);
			cfg.pin_ws = 25;
			cfg.pin_bck = 26;
			cfg.pin_data = 22;
			cfg.sample_rate = 44100;
			cfg.i2s_format = I2S_LSB_FORMAT;
			i2s.begin(cfg);
			/*
			Default pins are as follows :
			WSEL  ->  25
			DIN   ->  22
			BCLK  ->  26
			*/
		#endif
		#ifdef AUDIOKIT
			auto cfg = i2s.defaultConfig();
			cfg.copyFrom(info);
			i2s.begin(cfg);
		#endif
		a2dp_sink.set_auto_reconnect(true); //Auto-reconnect
		a2dp_sink.set_on_connection_state_changed(connectionStateChanged);
		a2dp_sink.set_on_audio_state_changed(audioStateChanged);
		a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
		a2dp_sink.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE|ESP_AVRC_MD_ATTR_ARTIST|ESP_AVRC_MD_ATTR_ALBUM|ESP_AVRC_MD_ATTR_PLAYING_TIME);
		a2dp_sink.set_avrc_rn_play_pos_callback(avrc_rn_play_pos_callback,1);
		#ifdef AUDIOKIT
			a2dp_sink.start("MiNiPoD56");
		#else
			a2dp_sink.start("espiPod 2");
		#endif

		#ifdef LED_BUILTIN
			pinMode(LED_BUILTIN,OUTPUT);
			digitalWrite(LED_BUILTIN,LOW);
		#endif
	#endif

	#ifdef DEBUG_MODE
		Serial.setTxBufferSize(4096);
		Serial.begin(115200);
  	#endif
	#ifndef AUDIOKIT
		Serial2.setRxBufferSize(4096);
		Serial2.setTxBufferSize(4096);
		Serial2.begin(19200);
	#else
		// ipodSerial.setPins(19,22);
		// ipodSerial.setRxBufferSize(4096);
		// ipodSerial.setTxBufferSize(4096);
		// ipodSerial.begin(19200);
		// digitalWrite(LED_BUILTIN,HIGH);
		Serial.setRxBufferSize(4096);
		Serial.setTxBufferSize(4096);
		Serial.begin(19200);
		//Serial.println(__TIME__);
	#endif
 	
	//Prep and start up espod
	espod.attachPlayControlHandler(playStatusHandler);

	#ifdef ENABLE_A2DP
	//Let's wait for something to start before we enable espod and start the game.
		while(a2dp_sink.get_connection_state()!=ESP_A2D_CONNECTION_STATE_CONNECTED) {
			delay(10);
		}
		//a2dp_sink.play(); //Essential to attempt auto-start. Creates issues with Offline mode on spotify
		delay(500);
		
	#endif
}

void loop() {
	if(espodRefreshTimer) {
		espod.refresh();
	}
}
