#include <Arduino.h>
#include <SD_MMC.h>  // 🌟 내장 SD 카드 전용 라이브러리로 교체 🌟
#include <Wire.h>
#include <U8g2lib.h>
#include <vector>
#include <FastLED.h>
#include <ArduinoJson.h>

// =========================================================================
// 1. 하드웨어 핀 정의 (S3 CAM 안전 핀맵 적용)
// =========================================================================
#define OLED_SDA 1
#define OLED_SCL 2

#define ENC_CLK  47
#define ENC_DT   14
#define ENC_SW   42

#define LED_PIN  21       
#define NUM_LEDS 61       // 🌟 61건반 1:1 매핑 🌟
#define BRIGHTNESS 100
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// =========================================================================
// 2. 객체 선언 (OLED 핀 번호 명시 유지)
// =========================================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ OLED_SCL, /* data=*/ OLED_SDA);

CRGB leds[NUM_LEDS];
CRGB targetLeds[NUM_LEDS];                  // 🌟 LED가 최종적으로 가져야 할 목표 색상
unsigned long ledCooldownEnd[NUM_LEDS];     // 🌟 연타 시 시각적 공백을 위한 개별 초시계

enum SystemState { 
    CHOOSE_SONG, CHOOSE_SPEED, CHOOSE_RANGE_START, CHOOSE_RANGE_END, 
    CONFIRM_PLAY, PLAYING_MUSIC, POST_PLAYBACK 
};
SystemState currentState = CHOOSE_SONG;

std::vector<String> songList;
int currentSongIdx = 0;

float speedOptions[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
int currentSpeedIdx = 3; 

int startMeasure = 1;
int endMeasure = 1;
int totalMeasures = 60; 

int confirmMenuIdx = 0; 
int postMenuIdx = 0;

// 인코더 변수
int encoderDelta = 0;
bool encoderClicked = false;
int lastClkState;
int lastBtnState = HIGH;
unsigned long lastButtonPress = 0;

// --- 재생용 데이터 구조체 ---
struct NoteEvent {
    int pitch; 
    char hand; 
};

struct TimeEvent {
    float t; 
    std::vector<NoteEvent> notes;
};

// PSRAM에 올라갈 전체 타임라인 배열
std::vector<TimeEvent> playbackTimeline;

// 재생 제어 변수
unsigned long playStartTime = 0;
float startTimeOffset = 0.0;
float endTimeTarget = 0.0;
int currentEventIdx = 0;
float currentBPM = 120.0; 

// =========================================================================
// 3. 주요 기능 함수 구현
// =========================================================================

void loadSongListFromFS() {
    songList.clear();
    
    // 🌟 내장 SD 카드에서 읽어오기 🌟
    File root = SD_MMC.open("/json_files");
    if (!root || !root.isDirectory()) {
        songList.push_back("No Folder!");
        return;
    }
    
    File file = root.openNextFile();
    while (file) {
        String fileName = String(file.name());
        
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) {
            fileName = fileName.substring(lastSlash + 1);
        }

        if (!file.isDirectory() && fileName.endsWith(".json")) {
            songList.push_back(fileName);
        }
        file = root.openNextFile();
    }
    
    if (songList.empty()) {
        songList.push_back("No JSON found");
    }
}

void readEncoder() {
    int currentClkState = digitalRead(ENC_CLK);
    if (currentClkState != lastClkState && currentClkState == LOW) {
        if (digitalRead(ENC_DT) != currentClkState) {
            encoderDelta = 1;  
        } else {
            encoderDelta = -1; 
        }
    }
    lastClkState = currentClkState;

    int btnState = digitalRead(ENC_SW);
    if (btnState == LOW && lastBtnState == HIGH) {
        if (millis() - lastButtonPress > 200) { 
            encoderClicked = true;
            lastButtonPress = millis();
        }
    }
    lastBtnState = btnState;
}

void updateDisplay() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf); 

    switch (currentState) {
        case CHOOSE_SONG:
            u8g2.drawStr(0, 10, "[1/5] Select Song");
            u8g2.setCursor(0, 28); u8g2.print("> " + songList[currentSongIdx]); break;
        case CHOOSE_SPEED:
            u8g2.drawStr(0, 10, "[2/5] Select Speed");
            u8g2.setCursor(0, 28); u8g2.print("> Speed: "); u8g2.print(speedOptions[currentSpeedIdx]); u8g2.print("x"); break;
        case CHOOSE_RANGE_START:
            u8g2.drawStr(0, 10, "[3/5] Start Measure"); u8g2.setCursor(0, 28);
            if (endMeasure == totalMeasures) u8g2.printf("> Range: [%d] - End", startMeasure);
            else u8g2.printf("> Range: [%d] - %d", startMeasure, endMeasure); break;
        case CHOOSE_RANGE_END:
            u8g2.drawStr(0, 10, "[4/5] End Measure"); u8g2.setCursor(0, 28);
            if (endMeasure == totalMeasures) u8g2.printf("> Range: %d - [End]", startMeasure);
            else u8g2.printf("> Range: %d - [%d]", startMeasure, endMeasure); break;
        case CONFIRM_PLAY:
            u8g2.drawStr(0, 10, "[5/5] Ready to Play?");
            if (confirmMenuIdx == 0) u8g2.drawStr(0, 28, "> [PLAY]    MAIN");
            else u8g2.drawStr(0, 28, "  PLAY    > [MAIN]"); break;
        case PLAYING_MUSIC:
            u8g2.drawStr(0, 10, "Playing Now..."); 
            u8g2.drawStr(0, 28, "Press Btn to STOP"); 
            break;
        case POST_PLAYBACK:
            u8g2.drawStr(0, 10, "Finished.");
            if (postMenuIdx == 0) u8g2.drawStr(0, 28, "> [REPLAY]  MAIN");
            else u8g2.drawStr(0, 28, "  REPLAY  > [MAIN]"); break;
    }
    u8g2.sendBuffer();
}

bool loadPlaybackData(String fileName) {
    playbackTimeline.clear();
    
    // 🌟 내장 SD 카드에서 파일 읽기 🌟
    String filePath = "/json_files/" + fileName;
    File file = SD_MMC.open(filePath, FILE_READ);
    if (!file) return false;

    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return false;

    currentBPM = doc["metadata"]["bpm"] | 120.0;
    
    JsonArray timeline = doc["timeline"];
    for (JsonObject timeObj : timeline) {
        TimeEvent te;
        te.t = timeObj["t"];
        
        JsonArray notes = timeObj["n"];
        for (JsonObject n : notes) {
            NoteEvent ne;
            ne.pitch = n["p"];
            String handStr = n["h"];
            ne.hand = handStr.charAt(0);
            te.notes.push_back(ne);
        }
        playbackTimeline.push_back(te);
    }

    totalMeasures = doc["metadata"]["total_measures"] | 0;
    if (totalMeasures == 0 && playbackTimeline.size() > 0) {
        float lastEventTime = playbackTimeline.back().t;
        totalMeasures = (int)(lastEventTime / (4.0 * (60.0 / currentBPM))) + 1;
    } else if (totalMeasures == 0) totalMeasures = 60;

    return true;
}

float measureToSeconds(int measure, float bpm) { return (measure - 1) * 4.0 * (60.0 / bpm); }

// =========================================================================
// 4. 초기화 및 메인 루프
// =========================================================================

void setup() {
    Serial.begin(115200);
    delay(1000); 

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT, INPUT_PULLUP);
    pinMode(ENC_SW, INPUT_PULLUP);
    lastClkState = digitalRead(ENC_CLK);

    // 🌟 1. OLED 시작 🌟
    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "1. OLED OK");
    u8g2.sendBuffer();

    // 🌟 2. FastLED 시작 🌟
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear();
    FastLED.show();
    
    // 타겟 및 쿨다운 초기화
    for(int i = 0; i < NUM_LEDS; i++) {
        targetLeds[i] = CRGB::Black;
        ledCooldownEnd[i] = 0;
    }
    
    u8g2.drawStr(0, 25, "2. LED OK");
    u8g2.sendBuffer();

    // 🌟 3. 내장 SD_MMC 1-Bit 모드 초기화 🌟
    SD_MMC.setPins(39, 38, 40); 
    if (!SD_MMC.begin("/sdcard", true)) { 
        u8g2.clearBuffer();
        u8g2.drawStr(0, 20, "SD Mount Fail!");
        u8g2.drawStr(0, 35, "Insert SD Card");
        u8g2.sendBuffer();
        while(1); 
    }

    u8g2.drawStr(0, 40, "3. SD_MMC OK");
    u8g2.sendBuffer();
    delay(1000); 

    loadSongListFromFS();
    updateDisplay();
}

void loop() {
    readEncoder(); 
    
    if (encoderDelta != 0 || encoderClicked) {
        switch (currentState) {
            case CHOOSE_SONG: 
                if (encoderDelta != 0) currentSongIdx = (currentSongIdx + encoderDelta + songList.size()) % songList.size();
                if (encoderClicked) {
                    u8g2.clearBuffer(); u8g2.drawStr(0, 20, "Loading File..."); u8g2.sendBuffer();
                    if (loadPlaybackData(songList[currentSongIdx])) currentState = CHOOSE_SPEED;
                    else { u8g2.clearBuffer(); u8g2.drawStr(0, 20, "Load Failed!"); u8g2.sendBuffer(); delay(1000); }
                } break;
            case CHOOSE_SPEED: 
                if (encoderDelta != 0) { currentSpeedIdx += encoderDelta; if (currentSpeedIdx > 7) currentSpeedIdx = 0; if (currentSpeedIdx < 0) currentSpeedIdx = 7; }
                if (encoderClicked) { startMeasure = 1; endMeasure = totalMeasures; currentState = CHOOSE_RANGE_START; } break;
            case CHOOSE_RANGE_START: 
                if (encoderDelta != 0) { startMeasure += encoderDelta; if (startMeasure < 1) startMeasure = 1; if (startMeasure > totalMeasures) startMeasure = totalMeasures; if (startMeasure > endMeasure) endMeasure = startMeasure; }
                if (encoderClicked) currentState = CHOOSE_RANGE_END; break;
            case CHOOSE_RANGE_END: 
                if (encoderDelta != 0) { endMeasure += encoderDelta; if (endMeasure < startMeasure) endMeasure = startMeasure; if (endMeasure > totalMeasures) endMeasure = totalMeasures; }
                if (encoderClicked) { confirmMenuIdx = 0; currentState = CONFIRM_PLAY; } break;
            case CONFIRM_PLAY: 
                if (encoderDelta != 0) { confirmMenuIdx = (confirmMenuIdx + encoderDelta) % 2; if (confirmMenuIdx < 0) confirmMenuIdx = 1; }
                if (encoderClicked) {
                    if (confirmMenuIdx == 0) {
                        startTimeOffset = measureToSeconds(startMeasure, currentBPM); endTimeTarget = measureToSeconds(endMeasure + 1, currentBPM); 
                        currentEventIdx = 0; while (currentEventIdx < playbackTimeline.size() && playbackTimeline[currentEventIdx].t < startTimeOffset) currentEventIdx++;
                        
                        // 연주 시작 전 LED 배열 완전 초기화
                        for(int i = 0; i < NUM_LEDS; i++) { targetLeds[i] = CRGB::Black; ledCooldownEnd[i] = 0; }
                        
                        playStartTime = millis(); FastLED.clear(); FastLED.show(); encoderClicked = false; currentState = PLAYING_MUSIC; updateDisplay(); 
                    } else { currentSongIdx = 0; currentState = CHOOSE_SONG; }
                } break;
            case POST_PLAYBACK: 
                if (encoderDelta != 0) { postMenuIdx = (postMenuIdx + encoderDelta) % 2; if (postMenuIdx < 0) postMenuIdx = 1; }
                if (encoderClicked) {
                    if (postMenuIdx == 0) {
                        startTimeOffset = measureToSeconds(startMeasure, currentBPM); endTimeTarget = measureToSeconds(endMeasure + 1, currentBPM);
                        currentEventIdx = 0; while (currentEventIdx < playbackTimeline.size() && playbackTimeline[currentEventIdx].t < startTimeOffset) currentEventIdx++;
                        
                        // 연주 시작 전 LED 배열 완전 초기화
                        for(int i = 0; i < NUM_LEDS; i++) { targetLeds[i] = CRGB::Black; ledCooldownEnd[i] = 0; }

                        playStartTime = millis(); 
                        encoderClicked = false; 
                        currentState = PLAYING_MUSIC; 
                        updateDisplay(); 
                    } else { currentSongIdx = 0; currentState = CHOOSE_SONG; } 
                } break;
            case PLAYING_MUSIC: break; 
        }
        encoderDelta = 0;
        if(currentState != PLAYING_MUSIC) { encoderClicked = false; updateDisplay(); }
    }

    // ==========================================
    // 🌟 연주 중 실시간 제어 (Non-blocking Cooldown 적용) 🌟
    // ==========================================
    if (currentState == PLAYING_MUSIC) {
        if (encoderClicked) {
            FastLED.clear(); FastLED.show();
            u8g2.clearBuffer(); u8g2.drawStr(0, 20, "Playback Stopped"); u8g2.sendBuffer();
            delay(1000); postMenuIdx = 0; currentState = POST_PLAYBACK; updateDisplay(); encoderClicked = false; return;
        }

        float speedFactor = speedOptions[currentSpeedIdx];
        float currentTime = ((millis() - playStartTime) / 1000.0 * speedFactor) + startTimeOffset;
        
        bool ledsChanged = false;
        unsigned long currentMillis = millis();

        // 1. JSON 이벤트 파싱 후 targetLeds와 쿨다운 설정
        while (currentEventIdx < playbackTimeline.size() && playbackTimeline[currentEventIdx].t <= currentTime) {
            for (NoteEvent ne : playbackTimeline[currentEventIdx].notes) {
                int pitch = abs(ne.pitch); 
                
                // 🌟 MIDI 36(C2) ~ 96(C7) 처리 (61건반 수학적 1:1 직접 매핑) 🌟
                if (pitch >= 36 && pitch <= 96) {
                    int ledIdx = pitch - 36; // pitch 36은 led 0번, pitch 96은 led 60번
                    
                    if (ne.pitch > 0) { // ON 명령
                        targetLeds[ledIdx] = (ne.hand == 'r') ? CRGB::Red : CRGB::Blue;
                    } else { // OFF 명령
                        targetLeds[ledIdx] = CRGB::Black;
                        // 🌟 시각적 타격감을 위한 60ms 강제 소등 쿨다운 🌟
                        ledCooldownEnd[ledIdx] = currentMillis + (60 / speedFactor); 
                    }
                }
            }
            currentEventIdx++;
        }

        // 2. targetLeds와 쿨다운을 비교하여 실제 LED에 적용
        for (int i = 0; i < NUM_LEDS; i++) {
            // 쿨다운(초시계)이 아직 안 끝났으면 무조건 검은색, 끝났으면 본래 타겟 색상
            CRGB desiredColor = (currentMillis < ledCooldownEnd[i]) ? CRGB::Black : targetLeds[i];
            
            // 현재 칠해져 있는 색과 다를 때만 업데이트를 진행
            if (leds[i] != desiredColor) {
                leds[i] = desiredColor;
                ledsChanged = true;
            }
        }

        // 색상이 변경되었을 때만 물리적인 신호 전송
        if (ledsChanged) {
            FastLED.show();
        }

        // 3. 곡 종료 확인
        if (currentEventIdx >= playbackTimeline.size() || currentTime >= endTimeTarget) {
            FastLED.clear(); FastLED.show(); postMenuIdx = 0; currentState = POST_PLAYBACK; updateDisplay();
        }
    }
}