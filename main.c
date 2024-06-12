static __attribute__((section (".noinit")))char losabuf[4096];

#include <lcd.h>
#include <math.h>
#include <stdio.h>
#include <float.h>
#include <string.h>
#include <stdarg.h>

#include <hardware/i2c.h>
#include <hardware/adc.h>
#include <hardware/rtc.h>
#include <hardware/gpio.h>
#include <hardware/sync.h>
#include <hardware/clocks.h>
#include <hardware/interp.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>

#include <pico/time.h>
#include <pico/types.h>
#include <pico/stdlib.h>
#include <pico/bootrom.h>
#include <pico/i2c_slave.h>
#include <pico/binary_info.h>
#include <pico/util/datetime.h>
#include <pico/bootrom/sf_table.h>

//#define GYRO_6AXIS            // ジャイロセンサ利用フラグ

#ifdef GYRO_6AXIS
#include <QMI8658.h>            // ジャイロセンサライブラリ
#endif

#include "w.h"
#include "lib/draw.h"
#include "img/font34.h"
#include "CST816S.h"           // LCDタッチ液晶ライブラリ

typedef struct {
  char mode[8];
  datetime_t dt;
  uint8_t theme;
  uint8_t editpos;
  uint8_t BRIGHTNESS;

  bool sensors;
  bool gyrocross;
  bool bender;
  bool SMOOTH_BACKGROUND;
  bool INSOMNIA;
  bool DYNAMIC_CIRCLES;
  bool DEEPSLEEP;
  bool is_sleeping;
  bool highpointer;
  bool alphapointer;
  bool clock;
  bool rotoz;
  bool rota;
  float fspin;
  uint8_t pstyle;
  int8_t spin;
  uint8_t texture;
  uint8_t configpos;
  uint8_t conf_bg;
  uint8_t conf_phour;
  uint8_t conf_pmin;
  uint8_t scandir;
  bool dither;
  uint8_t dummy;
  uint8_t save_crc;
} LOSA_t;

static LOSA_t* plosa=(LOSA_t*)losabuf;
#define LOSASIZE (&plosa->dummy - &plosa->theme)

#define SCRSAV 100			// 1st screensaver
int16_t screensaver=SCRSAV;
bool deepsleep=false;

volatile uint8_t flag_touch = 0;
extern uint8_t LCD_RST_PIN;

uint8_t* b0=NULL;
uint32_t* b1=NULL;

//one button /
#define QMIINT1 23
#define CBUT_TOUCH 16
uint8_t CBUT0 = 22;
bool fire_pressed=false;

bool fire=false;
bool ceasefire=false;

static int LCD_ADDR = 0x6b;

bool reserved_addr(uint8_t addr) {
  return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

bool i2c_scan(){
	printf("\nI2C Bus Scan \n");
	bool b_cst816_enable = false;
	printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
	for (int addr = 0; addr < (1 << 7); ++addr) {
			if (addr % 16 == 0) {					printf("%02x ", addr);			}
			int ret;
			uint8_t rxdata;
			if (reserved_addr(addr))
					ret = PICO_ERROR_GENERIC;
			else
					ret = i2c_read_blocking(I2C_PORT, addr, &rxdata, 1, false);

			if(ret >= 0 && addr == CST816_ADDR){
				b_cst816_enable = true;
				CBUT0 = CBUT_TOUCH;
				LCD_RST_PIN = 13;
			}
			printf(ret < 0 ? "." : "@");
			printf(addr % 16 == 15 ? "\n" : "  ");
	}
	return b_cst816_enable;
}

#define MS 1000
#define US 1000000
#define BUTD 500  // delay between possible button presses (default: 500, half of a second)
uint32_t rebootcounter = 0;
uint32_t button0_time=0;

void gpio_callback(uint gpio, uint32_t events) {
	if(events&GPIO_IRQ_EDGE_RISE){
		if(gpio==CBUT0){ceasefire=true;fire_pressed=false;rebootcounter=0;}
		if(gpio==QMIINT1){ deepsleep=false; }
		if(gpio==Touch_INT_PIN){
			deepsleep=false;
			flag_touch = 1;
		}
	}

	if(events&GPIO_IRQ_EDGE_FALL){
		if(gpio==CBUT0 && !fire && (((time_us_32()-button0_time)/MS)>=BUTD)){ceasefire=false;fire=true;button0_time = time_us_32();fire_pressed=true;}
	}

}

void draw_background() { }

void draw_gfx(){ }

void draw_text(){ }

typedef struct
{
	uint8_t click;
	int8_t pointer_x;
	int8_t pointer_y;
	int8_t wheel_h;
	int8_t wheel_v;
} simple_pointer_data_t;

typedef enum {
	NONE_CLICK,     // 0x00
	LEFT_CLICK,     // 0x01
	RIGHT_CLICK,    // 0x02
	MIDDLE_CLICK,   // 0x03
	CMD_POINTER,    // 0x04
	NOCHANGE_CLICK, // 0x05
} CLICK_I2C;

volatile uint8_t flag_event = 0;
volatile simple_pointer_data_t i2c_buf = {0};

typedef enum {
  MODE_NONE,
  MODE_TOUCHING,
  MODE_SCROLL_Y,
  MODE_SCROLL_X,
  MODE_DRAG,
  MODE_RELEASE,
  MODE_CLICK_LEFT,
  MODE_CLICK_RIGHT,
} TOUCH_MODE;

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240

// 画面タッチ位置 ////////////////////////////////////////////
#define TRIGGER_SG_LEFT     30 // 設定画面を出す操作のタッチ位置 LEFT
#define TRIGGER_SG_RIGHT   190 // 設定画面を出す操作のタッチ位置 RIGHT

#define SCROLL_Y_LIMIT_X         190 // 縦スクロール判定位置
#define CLICK_RIGHT_LIMIT_X       70 // 右クリック判定位置

#define SCROLL_X_LIMIT_Y         190 // 横スクロール判定位置
#define DRAG_LIMIT_Y              70 // ドラッグ開始判定位置

//////////////////////////////////////////////////////////////

#define CLICK_RELEASE_COUNT_LIMIT  5 // クリック判定
#define TOUCH_START_MSEC_LIMIT    30 // 新しいタッチ開始の判定msec
#define DRAG_UNDER_LIMIT_MSEC    300 // ドラッグ開始から解除までの最低msec

#define FLASH_TARGET_OFFSET 0x1F0000 // W25Q16JVの最終ブロック(Block31)のセクタ0の先頭アドレス

const char POS_X[3] = { 40, 40, 40};
const char POS_Y[3] = { 40, 60, 80};

char g_text_buf[3][128] = {{0}};
char g_old_text_buf[3][128] = {{0}};

void truncateString(char* text, int maxLength) {
  if (strlen(text) > maxLength) {
    text[maxLength] = '\0'; // 文字列をmaxLengthの長さに切り詰める
  }
}

typedef enum {
	SG_SLEEP,
	SG_DRUG_DIR,
	SG_DRUG_LEN,
	SG_RIGHT_CLICK_DIR,
	SG_RIGHT_CLICK_LEN,
	SG_SCROLL_Y_DIR,
	SG_SCROLL_Y_LEN,
	SG_SCROLL_X_DIR,
	SG_SCROLL_X_LEN,
	SG_NUM,
} SG_ITEM;

uint8_t g_sg_data[SG_NUM];

/** フラッシュに設定保存 **/
static void save_sg_to_flash(void) {
	uint32_t ints = save_and_disable_interrupts(); // 割り込み無効にする
	flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE); // Flash消去
	flash_range_program(FLASH_TARGET_OFFSET, g_sg_data, FLASH_PAGE_SIZE); // Flash書き込み
	restore_interrupts(ints); // 割り込みフラグを戻す
}

/** フラッシュから設定読み込み **/
void load_sg_from_flash(void) {
	const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);

	for(int i=0; i < SG_NUM; i++ ) {
		g_sg_data[i] = flash_target_contents[i];
		printf("SG %2d = %d\r\n", i, g_sg_data[i]);
	}
}



void lcd_text_draw(uint16_t lcd_color) {

	// 線を描画
	lcd_line(   SCROLL_Y_LIMIT_X,                0,    SCROLL_Y_LIMIT_X,     SCREEN_WIDTH, BLUE,   3);
	lcd_line(                  0, SCROLL_X_LIMIT_Y,       SCREEN_HEIGHT, SCROLL_X_LIMIT_Y, ORANGE, 3);
	lcd_line(CLICK_RIGHT_LIMIT_X,                0, CLICK_RIGHT_LIMIT_X,     SCREEN_WIDTH, RED,    3);
	lcd_line(                  0,     DRAG_LIMIT_Y,       SCREEN_HEIGHT,     DRAG_LIMIT_Y, YELLOW, 3);

	// 文字列データを表示
	for(int i=0; i<sizeof(g_text_buf)/sizeof(*g_text_buf); i++ ) {
		if(strcmp(g_text_buf[i], g_old_text_buf[i]) != 0) {
			// 古いのを消す
			lcd_str(POS_X[i], POS_Y[i]+1, g_old_text_buf[i], &Font20, lcd_color, WHITE);
		}

		// 表示する
		lcd_str(POS_X[i], POS_Y[i]+1, g_text_buf[i], &Font20, WHITE, lcd_color);

		// 古い文字列をバックアップ
		sprintf(g_old_text_buf[i], "%s", g_text_buf[i]);
	}
}

void lcd_text_set(int row, uint16_t lcd_color, const char *format, ...) {
	char text_buf[128]; // 出力用のバッファ
	va_list args;  // 可変引数リスト
	va_start(args, format); // 可変引数リストの初期化
	vsnprintf(text_buf, sizeof(text_buf), format, args); // フォーマットに従って文字列をバッファに書き込む
	va_end(args); // 可変引数リストのクリーンアップ

	printf("PLM %s\r\n", text_buf); // コンソールログ出力
		
	truncateString(text_buf, 12); // LCD表示用に長さを制限
	
	sprintf(g_text_buf[row-1], "%s", text_buf); // 表示配列にセット
}

void send_pointer() {
//	printf("send click=%d x=%d y=%d", i2c_buf.click, i2c_buf.pointer_x, i2c_buf.pointer_y);

	int n = 0;
	int i;
	uint8_t *raw_buf = (uint8_t *)&i2c_buf;

	i2c_write_blocking(i2c0, LCD_ADDR, &raw_buf[0], 1, true); 
	for (i = 1; i < 5; i++) {
		i2c_write_blocking(i2c0, LCD_ADDR, &raw_buf[i], 1, true); 
		raw_buf[i] = 0;
	}
}

void recv_event(i2c_inst_t *i2c) {
	uint8_t cmd = i2c_read_byte_raw(i2c);
	
	switch(cmd) {
		case CMD_POINTER:
			flag_event=1;
			break;
	}
	while(true) {
		uint8_t b = i2c_read_byte_raw(i2c);
		if(b == 0) {
			break;
		}
	}
}

void send_event() {
	if(flag_event) {
		send_pointer();
		flag_event = 0;
	}
}

static void i2c_slave_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
	switch (event) {
		case I2C_SLAVE_RECEIVE: // master has written some data
			recv_event(i2c);
			break;
		case I2C_SLAVE_REQUEST: // master is requesting data
			send_event();
			break;
		case I2C_SLAVE_FINISH: // master has signalled Stop / Restart
			  break;
		default:
			  break;
	}
}


int abs_value(int a, int b) {
	return abs(abs(a) - abs(b));
}

// フラッシュメモリのCSピンを定義
const uint FLASH_CS_PIN = 1;

/** 初期化処理 **/
void init() {
	sleep_ms(100);  // "Rain-wait" wait 100ms after booting (for other chips to initialize)
	rtc_init();
	stdio_init_all();

	// USB接続確立待ち
	////stdio_usb_init();
	//int i = 0;
	//while (i++ < 10) {
	//	if (stdio_usb_connected())
	//		break;
	//	sleep_ms(250);
	//}

	plosa->dummy=0;

	if(plosa->BRIGHTNESS < 10)plosa->BRIGHTNESS = 10;
	if(plosa->BRIGHTNESS > 100)plosa->BRIGHTNESS = 100;

	// I2C Master(LCDとの通信）
	i2c_init(I2C_PORT, 100 * 1000);

	gpio_set_function(DEV_SDA_PIN, GPIO_FUNC_I2C);
	gpio_set_function(DEV_SCL_PIN, GPIO_FUNC_I2C);
	gpio_pull_up(DEV_SDA_PIN);
	gpio_pull_up(DEV_SCL_PIN);
   
	// I2C Slave(メインのpico側への通信） 
	i2c_slave_init(i2c0, 0x0B, &i2c_slave_handler);
	gpio_set_function(16, GPIO_FUNC_I2C);
	gpio_set_function(17, GPIO_FUNC_I2C);
	gpio_pull_up(16);
	gpio_pull_up(17);

	while(true) {
		if(i2c_scan()) {
			break;
		}
		sleep_ms(1000);
	}

	lcd_init();

	plosa->scandir&=0x00; // 画面上向き
	lcd_setatt(plosa->scandir&0x03);

	lcd_make_cosin();
	lcd_set_brightness(plosa->BRIGHTNESS);
	printf("%02d-%02d-%04d %02d:%02d:%02d [%d]\n",plosa->dt.day,plosa->dt.month,plosa->dt.year,plosa->dt.hour,plosa->dt.min,plosa->dt.sec,plosa->dt.dotw);
	printf("mode='%s'\n",plosa->mode);
	printf("LOSASIZE=%d\n",LOSASIZE);
	b0 = malloc(LCD_SZ);
	b1 = (uint32_t*)b0;
	if(b0==0){printf("b0==0!\n");}
	uint32_t o = 0;
	lcd_setimg((uint16_t*)b0);

	printf("init realtime clock\n");
	rtc_set_datetime(&plosa->dt);
	printf("init realtime clock done\n");

	gpio_init(QMIINT1);
	gpio_set_dir(QMIINT1,GPIO_IN);
	gpio_set_irq_enabled_with_callback(QMIINT1, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_callback);

	gpio_init(Touch_INT_PIN);
	gpio_pull_up(Touch_INT_PIN);
	gpio_set_dir(Touch_INT_PIN,GPIO_IN);
	gpio_set_irq_enabled(Touch_INT_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
	CST816S_init(CST816S_Point_Mode);

#ifdef GYRO_6AXIS
	QMI8658_init();
#endif

	lcd_clr(BLACK); // 液晶クリア
}

// 座標構造体
typedef struct {
  int16_t x;
  int16_t y;
}axis_t;

const axis_t axis_0 = {0}; // 初期化用構造体

void i2c_data_set(int click, int x, int y, int h, int v) {
	flag_event = 1;
	//printf("i2c_data_set click=%d\r\n", click);
	i2c_buf.click = click == NOCHANGE_CLICK ? i2c_buf.click : click;
	i2c_buf.pointer_x = x;
	i2c_buf.pointer_y = y;
	i2c_buf.wheel_h = h;
	i2c_buf.wheel_v = v;
}

// 画面方向に合わせてX,Y座標を補正して取得
axis_t axis_rotate() {
	CST816S_Get_Point(); // 座標を取得
	axis_t axis_cur;
	axis_cur.x = (int16_t)Touch_CTS816.x_point;
	axis_cur.y = (int16_t)Touch_CTS816.y_point;
	if(plosa->scandir==1){
	      int16_t tt = axis_cur.x; axis_cur.x = axis_cur.y; axis_cur.y = tt;
	      axis_cur.y = LCD_H - axis_cur.y;
	}else if(plosa->scandir==2){
	      axis_cur.x = LCD_W - axis_cur.x;
	      axis_cur.y = LCD_H - axis_cur.y;
	}else if(plosa->scandir==3){
	      int16_t tt = axis_cur.x; axis_cur.x = axis_cur.y; axis_cur.y = tt;
	      axis_cur.x = LCD_W - axis_cur.x;
	}
	return axis_cur;
}

/** 座標の以前からの移動量の計算 **/
axis_t get_axis_delta(axis_t axis_cur, axis_t axis_old, double z) {

	axis_t axis_delta;
				
	axis_delta.x = axis_old.x == 0 ? 0 : axis_cur.x - axis_old.x;
	axis_delta.y = axis_old.y == 0 ? 0 : axis_cur.y - axis_old.y;
	
	axis_delta.x = ceil(axis_delta.x * z);
	axis_delta.y = ceil(axis_delta.y * z);

	return axis_delta;
}

///////////////////////////////
// 設定画面系
///////////////////////////////
#define SG_TITLE_X     60
#define SG_TITLE_Y     10
char g_sg_title_buf[20] = {0};

#define SG_VALUE_X     80
#define SG_VALUE_Y     90
char g_sg_value_buf[20] = {0};

#define SG_ITEM_HEIGHT 30
#define SG_ITEM_WIDTH  80

// 設定画面のボタンのフレーム
int SG_PREV[4] = { 30,  30,  30 + SG_ITEM_WIDTH,  30 + SG_ITEM_HEIGHT};
int SG_NEXT[4] = {120,  30, 120 + SG_ITEM_WIDTH,  30 + SG_ITEM_HEIGHT};
int SG_DOWN[4] = { 30, 150,  30 + SG_ITEM_WIDTH, 150 + SG_ITEM_HEIGHT};
int SG_UP[4]   = {120, 150, 120 + SG_ITEM_WIDTH, 150 + SG_ITEM_HEIGHT};

void lcd_frame_set(int frame[], int16_t lcd_color) {
	lcd_frame(frame[0], frame[1], frame[2], frame[3], lcd_color, 1);
}

/** 指定座標がフレーム範囲に入ってるかチェック **/
bool is_frame_touch(int frame[], axis_t axis_cur) {
	if(
		frame[0]   <= axis_cur.x && 
		frame[1]   <= axis_cur.y && 
		axis_cur.x <= frame[2] && 
		axis_cur.y <= frame[3]
	) {
		return true;
	}
	return false;
}

void lcd_sg_draw() {
	lcd_clr(BLACK);

	lcd_str(SG_TITLE_X, SG_TITLE_Y, g_sg_title_buf, &Font20, WHITE, BLACK);
	lcd_str(SG_VALUE_X, SG_VALUE_Y, g_sg_value_buf, &Font20, WHITE, BLACK);

	lcd_frame_set(SG_PREV, WHITE); // 
	lcd_frame_set(SG_NEXT, WHITE); // 
	lcd_frame_set(SG_UP  , WHITE); // 
	lcd_frame_set(SG_DOWN, WHITE); // 
}

void lcd_sg_set(int place, char *text) {
}

void sg_title_set(int sg_no) {
	// 設定項目タイトル
	switch(sg_no) {
		case SG_SLEEP:
			sprintf(g_sg_title_buf, "SLEEP");
			break;
		default:
			break;
	}
}


/** 設定画面処理ループ **/
void sg_display_loop() {
	/////////// 反転テスト
	//g_sg_data[SG_SLEEP] = !g_sg_data[SG_SLEEP];
	//printf("g_sg_data[SG_SLEEP]=%d\r\n", g_sg_data[SG_SLEEP]);
	
	axis_t axis_cur;				// 現在座標
	int touch_mode = 0;				// 操作モード
	int sg_no = SG_SLEEP;				// 設定項目番号
	uint32_t last_touch_time = time_us_32();	// 最後に触った時刻

	lcd_clr(BLACK);		// 画面クリア
	sg_title_set(sg_no);	// 設定項目タイトル
	lcd_sg_draw();

	while(true) {
		// 軌跡を表示
		lcd_bez3circ(axis_cur.x, axis_cur.y, 3, GREEN, 3, 0, 0);

		// タッチが行われた場合
		if(flag_touch){
			axis_cur = axis_rotate(); // 画面方向に合わせてX,Y座標を補正して取得
		
			// タッチをしはじめた時
			if( ((time_us_32()-last_touch_time)/MS) > TOUCH_START_MSEC_LIMIT){
				printf("TOUCH START\r\n");
				touch_mode = MODE_TOUCHING;
			} 
				
			last_touch_time = time_us_32();
			flag_touch = 0;
		} else {
			touch_mode = MODE_NONE;
		} // if(flag) END

		// 各種操作
		bool b_op = false; // 操作したかどうかフラグ
		if(touch_mode == MODE_TOUCHING) {
			if(is_frame_touch(SG_NEXT, axis_cur)) {
				lcd_clr(BLACK); // 画面クリア
				b_op = true;
				return;
			}

			uint8_t i_tmp = g_sg_data[SG_SLEEP];
			if(is_frame_touch(SG_DOWN, axis_cur)) {
				if(i_tmp > 0) {
					i_tmp --;
				}
				b_op = true;
			}
			if(is_frame_touch(SG_UP, axis_cur)) {
				if(i_tmp < 30) {
					i_tmp ++;
				}
				b_op = true;
			}
			g_sg_data[SG_SLEEP] = i_tmp;
			sprintf(g_sg_value_buf, "%d", g_sg_data[SG_SLEEP]);
		}
		if(b_op) {
			sg_title_set(sg_no); // 設定項目タイトル
			lcd_sg_draw();
		}
		lcd_display(b0);
		sleep_ms(100);	
	}

	save_sg_to_flash();
}

/** メイン処理ループ **/
void mouse_display_loop() {
	axis_t axis_cur;				// 現在座標
	axis_t axis_old;				// 一つ前の座標
	axis_t axis_delta;				// 移動量
	axis_t axis_touch;				// タッチ開始時の座標

	int touch_mode = 0;				// 操作モード
	int release_cnt = 0;				// タッチを離してる間のカウント値
	int click_stat_old = NONE_CLICK;		// クリック状態の一つ前の状態
	uint16_t lcd_color = BLACK;			// LCD背景色
	uint32_t last_touch_time = time_us_32();	// 最後に触った時刻
	uint32_t start_drag_time = time_us_32();	// ドラッグ開始時刻

	int sg_trigger_cnt = 0;				// 設定画面の操作カウント
	uint32_t sg_trigger_time = time_us_32();	// 設定画面の操作開始時刻

	printf("loop start !!\r\n");
	
	while(true){
		// クリック状態が変わったら背景色を設定(画面クリア）
		if(i2c_buf.click != click_stat_old) {
			printf("lcd_color change=%d i2c_buf.click=%d click_stat_old=%d\r\n", lcd_color, i2c_buf.click, click_stat_old);
			lcd_clr(lcd_color);
		}
		click_stat_old = i2c_buf.click;

		// タッチが行われた場合
		if(flag_touch){
			release_cnt++;
		
			screensaver=SCRSAV;
			if(plosa->is_sleeping){
			      lcd_set_brightness(plosa->BRIGHTNESS);
			      lcd_sleepoff();
			}
			plosa->is_sleeping=false;

			axis_cur = axis_rotate(); // 画面方向に合わせてX,Y座標を補正して取得
		
			// 軌跡を表示
			lcd_bez3circ(axis_cur.x, axis_cur.y, 3, GREEN, 3, 0, 0);

			// 座標表示
			lcd_text_set(1, lcd_color, "X:%03d Y:%03d", axis_cur.x, axis_cur.y);
		
			// タッチをしはじめた時
			if( ((time_us_32()-last_touch_time)/MS) > TOUCH_START_MSEC_LIMIT){
				printf("TOUCH START\r\n");
				touch_mode = MODE_TOUCHING;
		
				// 縦スクロール判定
				if(axis_cur.x > SCROLL_Y_LIMIT_X) {
					printf("start scroll y\r\n");
					touch_mode = MODE_SCROLL_Y;
				}
				
				// 横スクロール判定
				if(axis_cur.y > SCROLL_X_LIMIT_Y) {
					printf("start scroll x\r\n");
					touch_mode = MODE_SCROLL_X;
				}
				
				// タップしてる軌跡を消す処理
				printf("line clear: %08x %d\r\n",last_touch_time,((time_us_32()-last_touch_time)/MS));
				lcd_clr(lcd_color);
				
				release_cnt = 0;
		
				axis_touch = axis_cur;
			} 
				
			// ドラッグ開始判定
			if(release_cnt > 20 && abs_value(axis_touch.x, axis_cur.x) < 5 && abs_value(axis_touch.y, axis_cur.y) < 5 && axis_cur.y < DRAG_LIMIT_Y) {
				printf("drag start\r\n");
				touch_mode = MODE_DRAG;
			}
		
			// 右クリック判定
			if(release_cnt > 20 && abs_value(axis_touch.x, axis_cur.x) < 5 && abs_value(axis_touch.y, axis_cur.y) < 5 && axis_cur.x < CLICK_RIGHT_LIMIT_X) {
				printf("MODE_CLICK_RIGHT\r\n");
				touch_mode = MODE_CLICK_RIGHT;
			}

			// 設定画面呼び出し操作時間判定
			bool b_sg_trigger_time = (time_us_32()-sg_trigger_time)/MS < 3000;

			// 設定画面の呼び出し操作状態判定
			if(       sg_trigger_cnt == 0 && axis_cur.x < TRIGGER_SG_LEFT) {
				printf("sg_trigger_cnt=%d\r\n", sg_trigger_cnt);
				sg_trigger_cnt++;
			} else if(sg_trigger_cnt == 1 && axis_cur.x > TRIGGER_SG_RIGHT) {
				printf("sg_trigger_cnt=%d\r\n", sg_trigger_cnt);
				sg_trigger_cnt++;
			} else if(sg_trigger_cnt == 2 && axis_cur.x < TRIGGER_SG_LEFT) {
				printf("sg_trigger_cnt=%d\r\n", sg_trigger_cnt);
				sg_trigger_cnt++;
			} else if(sg_trigger_cnt == 3 && axis_cur.x > TRIGGER_SG_RIGHT) {
				printf("sg_trigger_cnt=%d\r\n", sg_trigger_cnt);
				if(b_sg_trigger_time) {
					lcd_text_set(3, lcd_color, "SG START");
					sg_display_loop(); // 設定画面呼び出し
					sg_trigger_cnt=0;
				}
			}

			// 設定画面の呼び出し操作時間切れ
			if(!b_sg_trigger_time) {
				// 設定操作開始状態クリア
				lcd_text_set(3, lcd_color, "");
				printf("sg_trigger clear!!\r\n");
				sg_trigger_cnt = 0;
				sg_trigger_time = time_us_32();
			}
		
			// 	
			last_touch_time = time_us_32();
			flag_touch = 0;
		} else {
			touch_mode = MODE_NONE;

			// スクリーンセーバー
			if(g_sg_data[SG_SLEEP]) {
				screensaver--;
				if(screensaver<=0){
					plosa->is_sleeping=true;
					screensaver=SCRSAV;
					lcd_set_brightness(0);
					lcd_sleepon();
				}
			}
		} // if(flag) END

		// 各種操作	
		switch(touch_mode) {
			case MODE_TOUCHING:
				//lcd_text_set(2, lcd_color, "TOUCHING");
				axis_delta = get_axis_delta(axis_cur, axis_old, 0.7);
				i2c_data_set(NOCHANGE_CLICK, axis_delta.x, axis_delta.y, 0, 0);
				axis_old = axis_cur;
				break;	
			case MODE_SCROLL_Y:
				if(axis_cur.x > SCROLL_Y_LIMIT_X) {
					axis_delta = get_axis_delta(axis_cur, axis_old, 0.5);
					lcd_text_set(2, lcd_color, "SCROLL Y dx:%d dy:%d", axis_delta.x, axis_delta.y);
					i2c_data_set(NOCHANGE_CLICK, 0, 0, 0, -axis_delta.y);
					last_touch_time = time_us_32();	// 最後に触った時刻

					axis_old = axis_cur;
				} else {
					touch_mode = MODE_TOUCHING;
				}
				break;	
			case MODE_SCROLL_X:
				if(axis_cur.y > SCROLL_X_LIMIT_Y) {
					axis_delta = get_axis_delta(axis_cur, axis_old, 0.5);
					lcd_text_set(2, lcd_color, "SCROLL X dx:%d dy:%d", axis_delta.x, axis_delta.y);
					i2c_data_set(NOCHANGE_CLICK, 0, 0, axis_delta.x, 0);
					last_touch_time = time_us_32();	// 最後に触った時刻
					axis_old = axis_cur;
				} else {
					touch_mode = MODE_TOUCHING;
				}
				break;	
			case MODE_DRAG:
				lcd_text_set(2, lcd_color, "DRAG");

				lcd_color = ORANGE;
				axis_delta = get_axis_delta(axis_cur, axis_old, 0.7);
				i2c_data_set(LEFT_CLICK, axis_delta.x, axis_delta.y, 0, 0);
				axis_old = axis_cur;

				start_drag_time = time_us_32(); // ドラッグ開始時刻保存

				break;	
			case MODE_CLICK_RIGHT:
				lcd_text_set(2, lcd_color, "CLICK RIGHT");
				lcd_color = BLACK;
				
				i2c_data_set(RIGHT_CLICK, 0, 0, 0, 0);
				sleep_ms(10);	
				i2c_data_set(NONE_CLICK, 0, 0, 0, 0);
	
				touch_mode = MODE_NONE;
				axis_touch = axis_0;
				axis_old   = axis_0;
				break;	
			case MODE_RELEASE:
				lcd_text_set(2, lcd_color, "TOUCH RELEASE");
				axis_old   = axis_0;
				break;	
			case MODE_NONE:
				axis_delta = get_axis_delta(axis_cur, axis_old, 1);

				uint32_t delta_drag_time = (time_us_32()-start_drag_time)/MS;
				
				if(axis_delta.x < 20 && axis_delta.y < 20 && release_cnt < CLICK_RELEASE_COUNT_LIMIT && delta_drag_time > DRAG_UNDER_LIMIT_MSEC) {
					// 左クリック
					lcd_text_set(2, lcd_color, "CLICK LEFT release_cnt=%d delta_drag_time=%d", release_cnt, delta_drag_time);
					lcd_color = BLACK;	
				
					i2c_data_set(LEFT_CLICK, 0, 0, 0, 0);
					sleep_ms(10);	
					i2c_data_set(NONE_CLICK, 0, 0, 0, 0);
	
					release_cnt = CLICK_RELEASE_COUNT_LIMIT;
				}
	
				axis_old   = axis_0;
				break;	
			default:
				axis_old   = axis_0;
				break;	
		}

		lcd_text_draw(lcd_color);
		lcd_display(b0);
	}
}

int main(void) {
	init();				// 初期化処理

	load_sg_from_flash();	// フラッシュから設定読み取り

	mouse_display_loop();	// メイン処理ループ
	
	return 0;
}

