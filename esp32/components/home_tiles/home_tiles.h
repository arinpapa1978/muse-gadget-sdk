// 우리 집 정보 타일 (Muse 휴대용 1.75인치 둥근 화면): 주식 · 날씨 · 교통 · 일정
// Muse 얼굴 타일 아래에 위로 밀면 차례로 나옴. 데이터는 Worker /api/gadget 에서 1분마다 (보고 있을 때만)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
// tv: Muse 의 타일 보기, face: 얼굴 타일. LVGL 잠금 상태에서 부름 (Muse UI 만들 때)
void home_tiles_create(lv_obj_t *tv, lv_obj_t *face);
// 우리 집 목소리 (home_tts.c): Muse 답변 글자 → Worker 음성 (16 kHz, 16bit, 모노)
enum { HOME_TTS_IDLE, HOME_TTS_RUNNING, HOME_TTS_DONE, HOME_TTS_FAILED };
bool home_tts_start(const char *text);           // 열쇠가 없으면 false (그때는 글자로만)
size_t home_tts_read(uint8_t *out, size_t cap);  // 받아 둔 음성 꺼내기 (기다리지 않음, 짝수 바이트)
int home_tts_state(void);                        // 받는 중이거나 남은 음성이 있으면 RUNNING
void home_tts_cancel(void);
// 시리얼 명령 ("home_key=…", "home_url=…", "home"): 처리했으면 true
bool home_tiles_console(const char *line);
#ifdef __cplusplus
}
#endif
