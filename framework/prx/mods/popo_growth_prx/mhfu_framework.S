	.set noreorder

#include "pspstub.s"

	STUB_START "mhfu_framework",0x40090000,0x000E0005
	STUB_FUNC  0xC84B540F,mhfu_register_event
	STUB_FUNC  0x6C92A360,mhfu_unregister_event
	STUB_FUNC  0x96EC37F0,mhfu_get_screen_state
	STUB_FUNC  0x8A5B5598,mhfu_get_area_index
	STUB_FUNC  0x8DA50767,mhfu_get_quest_timer
	STUB_FUNC  0xF0E6035E,mhfu_get_player_hp
	STUB_FUNC  0xDA661EBD,mhfu_get_sharpness_current
	STUB_FUNC  0xF454885B,mhfu_read_u8
	STUB_FUNC  0x8B1C3752,mhfu_read_u16
	STUB_FUNC  0x9AF99E85,mhfu_read_u32
	STUB_FUNC  0x94963936,mhfu_write_u8
	STUB_FUNC  0xCEED3E83,mhfu_write_u16
	STUB_FUNC  0xCE6A089F,mhfu_write_u32
	STUB_FUNC  0xC076763A,mhfu_log
	STUB_END
