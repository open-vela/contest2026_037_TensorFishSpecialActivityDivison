# vendor/allwinnertech/apps/deskmate/CMakeLists.txt

if(CONFIG_DESKMATE_APP)
  nuttx_add_application(
    NAME deskmate
    PRIORITY ${CONFIG_DESKMATE_APP_PRIORITY}
    STACKSIZE ${CONFIG_DESKMATE_APP_STACKSIZE}
    SRCS
      deskmate_main.c
      dm_state.c
      dm_face.c
      dm_roll.c
      dm_ui_focus.c
      dm_ui_shells.c
      dm_ui_health.c
      dm_ui_wifi.c dm_ui_appconn.c dm_bt_pair.c dm_ble_gatt.c dm_dict.c dm_ui_dict.c dm_voice.c dm_audio_vol.c dm_ui_vol.c dm_proactive.c dm_tts_voice.c dm_ui_voice.c dm_ui_about.c dm_audio_route.c dm_ui_recorder.c
      dm_wifi.c
      dm_ui_word.c
      dm_word_bank.c
      dm_ui_tools.c
      dm_ui_life.c
      dm_ui_fun.c
      dm_ui_med.c
      dm_ui_note.c
      dm_pinyin.c
      dm_ui_2048.c
      dm_ui_chat.c
      dm_chat_ai.c
      dm_ui_mbti.c
      dm_ui_remind.c
      dm_ai_permissions.c
      dm_ui_permissions.c
      dm_ai_log.c
      dm_ui_ai_log.c
      dm_ai_local_read.c
      dm_ai_data.c
      dm_assistant.c
      dm_ui_meeting.c
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
endif()
