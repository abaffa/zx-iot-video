#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "video_attr.h"
#include "iis_videosig.h"
#include "lcd_display.h"


static const char* TAG = "lcd_disp";

static void tftd_task(void*arg);
 

/**
 This code displays some fancy graphics on the 320x240 LCD on an ESP-WROVER_KIT board.
 This example demonstrates the use of both spi_device_transmit as well as
 spi_device_queue_trans/spi_device_get_trans_result and pre-transmit callbacks.

 Some info about the ILI9341/ST7789V: It has an C/D line, which is connected to a GPIO here. It expects this
 line to be low for a command and high for data. We use a pre-transmit callback here to control that
 line: every transaction has as the user-definable argument the needed state of the D/C line and just
 before the transaction is sent, the callback will set this line to the correct state.
*/

#define PIN_NUM_MISO 35//35//25
#define PIN_NUM_MOSI 18//18//23
#define PIN_NUM_CLK  19//15//19
#define PIN_NUM_CS   15//4//22

#define PIN_NUM_DC   5//5//21
#define PIN_NUM_RST  4//21//18
#define PIN_NUM_BCKL GPIO_NUM_MAX //23//5  NOT CONNECTED

//To speed up transfers, every SPI transfer sends a bunch of lines. This define specifies how many. More means more memory use,
//but less overhead for setting up / finishing transfers. Make sure 240 is dividable by this.
#define PARALLEL_LINES 16

/*
 The LCD needs a bunch of command/argument values to be initialized. They are stored in this struct.
*/
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} lcd_init_cmd_t;

typedef enum {
    LCD_TYPE_ILI = 1,
    LCD_TYPE_ST,
    LCD_TYPE_MAX,
} type_lcd_t;



/*
#define BLUE         	 0x001F  
#define BRED             0XF81F
#define GRED 			 0XFFE0
#define GBLUE			 0X07FF
#define RED           	 0xF800
#define MAGENTA       	 0xF81F
#define GREEN         	 0x07E0
#define CYAN          	 0x7FFF
#define YELLOW        	 0xFFE0
*/
//        actual_colour=0x081F; // green
        //actual_colour=0x03FF;  // yellow

#define WHITE         	 0xFFFF
#define BLACK         	 0x0000	  
#define BLUE         	 0x1F00  
#define GREEN         	 0xE007
#define RED           	 0x00F8


static uint16_t fg_colour=BLACK;
static uint16_t bg_colour=WHITE;

static uint16_t actual_colour=WHITE;


//Place data into DRAM. Constant data gets placed into DROM by default, which is not accessible by DMA.
DRAM_ATTR static const lcd_init_cmd_t st_init_cmds[]={
    /* Memory Data Access Control, MX=MV=1, MY=ML=MH=0, RGB=0 */
    {0x36, {(1<<5)|(1<<6)}, 1},
    /* Interface Pixel Format, 16bits/pixel for RGB/MCU interface */
    {0x3A, {0x55}, 1},
    /* Porch Setting */
    {0xB2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
    /* Gate Control, Vgh=13.65V, Vgl=-10.43V */
    {0xB7, {0x45}, 1},
    /* VCOM Setting, VCOM=1.175V */
    {0xBB, {0x2B}, 1},
    /* LCM Control, XOR: BGR, MX, MH */
    {0xC0, {0x2C}, 1},
    /* VDV and VRH Command Enable, enable=1 */
    {0xC2, {0x01, 0xff}, 2},
    /* VRH Set, Vap=4.4+... */
    {0xC3, {0x11}, 1},
    /* VDV Set, VDV=0 */
    {0xC4, {0x20}, 1},
    /* Frame Rate Control, 60Hz, inversion=0 */
    {0xC6, {0x0f}, 1},
    /* Power Control 1, AVDD=6.8V, AVCL=-4.8V, VDDS=2.3V */
    {0xD0, {0xA4, 0xA1}, 1},
    /* Positive Voltage Gamma Control */
    {0xE0, {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}, 14},
    /* Negative Voltage Gamma Control */
    {0xE1, {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}, 14},
    /* Sleep Out */
    {0x11, {0}, 0x80},
    /* Display On */
    {0x29, {0}, 0x80},
    {0, {0}, 0xff}
};

DRAM_ATTR static const lcd_init_cmd_t ili_init_cmds[]={
    /* Power contorl B, power control = 0, DC_ENA = 1 */
    {0xCF, {0x00, 0x83, 0X30}, 3},
    /* Power on sequence control,
     * cp1 keeps 1 frame, 1st frame enable
     * vcl = 0, ddvdh=3, vgh=1, vgl=2
     * DDVDH_ENH=1
     */
    {0xED, {0x64, 0x03, 0X12, 0X81}, 4},
    /* Driver timing control A,
     * non-overlap=default +1
     * EQ=default - 1, CR=default
     * pre-charge=default - 1
     */
    {0xE8, {0x85, 0x01, 0x79}, 3},
    /* Power control A, Vcore=1.6V, DDVDH=5.6V */
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
    /* Pump ratio control, DDVDH=2xVCl */
    {0xF7, {0x20}, 1},
    /* Driver timing control, all=0 unit */
    {0xEA, {0x00, 0x00}, 2},
    /* Power control 1, GVDD=4.75V */
    {0xC0, {0x26}, 1},
    /* Power control 2, DDVDH=VCl*2, VGH=VCl*7, VGL=-VCl*3 */
    {0xC1, {0x11}, 1},
    /* VCOM control 1, VCOMH=4.025V, VCOML=-0.950V */
    {0xC5, {0x35, 0x3E}, 2},
    /* VCOM control 2, VCOMH=VMH-2, VCOML=VML-2 */
    {0xC7, {0xBE}, 1},
    /* Memory access contorl, MX=MY=0, MV=1, ML=0, BGR=1, MH=0 */
    {0x36, {0x28}, 1},
    /* Pixel format, 16bits/pixel for RGB/MCU interface */
    {0x3A, {0x55}, 1},
    /* Frame rate control, f=fosc, 70Hz fps */
    {0xB1, {0x00, 0x1B}, 2},
    /* Enable 3G, disabled */
    {0xF2, {0x08}, 1},
    /* Gamma set, curve 1 */
    {0x26, {0x01}, 1},
    /* Positive gamma correction */
    {0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0X87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
    /* Negative gamma correction */
    {0XE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
    /* Column address set, SC=0, EC=0xEF */
    {0x2A, {0x00, 0x00, 0x00, 0xEF}, 4},
    /* Page address set, SP=0, EP=0x013F */
    {0x2B, {0x00, 0x00, 0x01, 0x3f}, 4},
    /* Memory write */
    {0x2C, {0}, 0},
    /* Entry mode set, Low vol detect disabled, normal display */
    {0xB7, {0x07}, 1},
    /* Display function control */
    {0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
    /* Sleep out */
    {0x11, {0}, 0x80},
    /* Display on */
    {0x29, {0}, 0x80},
    {0, {0}, 0xff},
};

/* Send a command to the LCD. Uses spi_device_polling_transmit, which waits
 * until the transfer is complete.
 *
 * Since command transactions are usually small, they are handled in polling
 * mode for higher speed. The overhead of interrupt transactions is more than
 * just waiting for the transaction to complete.
 */
void lcd_cmd(spi_device_handle_t spi, const uint8_t cmd)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=8;                     //Command is 8 bits
    t.tx_buffer=&cmd;               //The data is the cmd itself
    t.user=(void*)0;                //D/C needs to be set to 0
    ret=spi_device_polling_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
}

/* Send data to the LCD. Uses spi_device_polling_transmit, which waits until the
 * transfer is complete.
 *
 * Since data transactions are usually small, they are handled in polling
 * mode for higher speed. The overhead of interrupt transactions is more than
 * just waiting for the transaction to complete.
 */
void lcd_data(spi_device_handle_t spi, const uint8_t *data, int len)
{
    esp_err_t ret;
    spi_transaction_t t;
    if (len==0) return;             //no need to send anything
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=len*8;                 //Len is in bytes, transaction length is in bits.
    t.tx_buffer=data;               //Data
    t.user=(void*)1;                //D/C needs to be set to 1
    ret=spi_device_polling_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
}

//This function is called (in irq context!) just before a transmission starts. It will
//set the D/C line to the value indicated in the user field.
void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc=(int)t->user;
    gpio_set_level(PIN_NUM_DC, dc);
}

uint32_t lcd_get_id(spi_device_handle_t spi)
{
    //get_id cmd
    lcd_cmd(spi, 0x04);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length=8*3;
    t.flags = SPI_TRANS_USE_RXDATA;
    t.user = (void*)1;

    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    assert( ret == ESP_OK );

    return *(uint32_t*)t.rx_data;
}

#define CONFIG_LCD_TYPE_ILI9341 1

//Initialize the display
void lcd_init(spi_device_handle_t spi)
{
    int cmd=0;
    const lcd_init_cmd_t* lcd_init_cmds;

    //Initialize non-SPI GPIOs
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    if(PIN_NUM_BCKL!=GPIO_NUM_MAX) gpio_set_direction(PIN_NUM_BCKL, GPIO_MODE_OUTPUT);

    //Reset the display
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(100 / portTICK_RATE_MS);
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(100 / portTICK_RATE_MS);

    //detect LCD type
    uint32_t lcd_id = lcd_get_id(spi);
    int lcd_detected_type = 0;
    int lcd_type;

    printf("LCD ID: %08X\n", lcd_id);
    if ( lcd_id == 0 ) {
        //zero, ili
        lcd_detected_type = LCD_TYPE_ILI;
        printf("ILI9341 detected.\n");
    } else {
        // none-zero, ST
        lcd_detected_type = LCD_TYPE_ST;
        printf("ST7789V detected.\n");
    }

#ifdef CONFIG_LCD_TYPE_AUTO
    lcd_type = lcd_detected_type;
#elif defined( CONFIG_LCD_TYPE_ST7789V )
    printf("kconfig: force CONFIG_LCD_TYPE_ST7789V.\n");
    lcd_type = LCD_TYPE_ST;
#elif defined( CONFIG_LCD_TYPE_ILI9341 )
    printf("kconfig: force CONFIG_LCD_TYPE_ILI9341.\n");
    lcd_type = LCD_TYPE_ILI;
#endif
    if ( lcd_type == LCD_TYPE_ST ) {
        printf("LCD ST7789V initialization.\n");
        lcd_init_cmds = st_init_cmds;
    } else {
        printf("LCD ILI9341 initialization.\n");
        lcd_init_cmds = ili_init_cmds;
    }

    //Send all the commands
    while (lcd_init_cmds[cmd].databytes!=0xff) {
        lcd_cmd(spi, lcd_init_cmds[cmd].cmd);
        lcd_data(spi, lcd_init_cmds[cmd].data, lcd_init_cmds[cmd].databytes&0x1F);
        if (lcd_init_cmds[cmd].databytes&0x80) {
            vTaskDelay(100 / portTICK_RATE_MS);
        }
        cmd++;
    }

    ///Enable backlight
    if(PIN_NUM_BCKL!=GPIO_NUM_MAX) gpio_set_level(PIN_NUM_BCKL, 0);
}


/* To send a set of lines we have to send a command, 2 data bytes, another command, 2 more data bytes and another command
 * before sending the line data itself; a total of 6 transactions. (We can't put all of this in just one transaction
 * because the D/C line needs to be toggled in the middle.)
 * This routine queues these commands up as interrupt transactions so they get
 * sent faster (compared to calling spi_device_transmit several times), and at
 * the mean while the lines for next transactions can get calculated.
 */
static void send_lines(spi_device_handle_t spi, int ypos, uint16_t *linedata)
{
    esp_err_t ret;
    int x;
    //Transaction descriptors. Declared static so they're not allocated on the stack; we need this memory even when this
    //function is finished because the SPI driver needs access to it even while we're already calculating the next line.
    static spi_transaction_t trans[6];

    //In theory, it's better to initialize trans and data only once and hang on to the initialized
    //variables. We allocate them on the stack, so we need to re-init them each call.
    for (x=0; x<6; x++) {
        memset(&trans[x], 0, sizeof(spi_transaction_t));
        if ((x&1)==0) {
            //Even transfers are commands
            trans[x].length=8;
            trans[x].user=(void*)0;
        } else {
            //Odd transfers are data
            trans[x].length=8*4;
            trans[x].user=(void*)1;
        }
        trans[x].flags=SPI_TRANS_USE_TXDATA;
    }
    trans[0].tx_data[0]=0x2A;           //Column Address Set
    trans[1].tx_data[0]=0;              //Start Col High
    trans[1].tx_data[1]=0;              //Start Col Low
    trans[1].tx_data[2]=(320)>>8;       //End Col High
    trans[1].tx_data[3]=(320)&0xff;     //End Col Low
    trans[2].tx_data[0]=0x2B;           //Page address set
    trans[3].tx_data[0]=ypos>>8;        //Start page high
    trans[3].tx_data[1]=ypos&0xff;      //start page low
    trans[3].tx_data[2]=(ypos+PARALLEL_LINES)>>8;    //end page high
    trans[3].tx_data[3]=(ypos+PARALLEL_LINES)&0xff;  //end page low
    trans[4].tx_data[0]=0x2C;           //memory write
    trans[5].tx_buffer=linedata;        //finally send the line data
    trans[5].length=320*2*8*PARALLEL_LINES;          //Data length, in bits
    trans[5].flags=0; //undo SPI_TRANS_USE_TXDATA flag

    //Queue all transactions.
    for (x=0; x<6; x++) {
        ret=spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }

    //When we are here, the SPI driver is busy (in the background) getting the transactions sent. That happens
    //mostly using DMA, so the CPU doesn't have much to do here. We're not going to wait for the transaction to
    //finish because we may as well spend the time calculating the next line. When that is done, we can call
    //send_line_finish, which will wait for the transfers to be done and check their status.
}


static void send_line_finish(spi_device_handle_t spi)
{
    spi_transaction_t *rtrans;
    esp_err_t ret;
    //Wait for all 6 transactions to be done and get back the results.
    for (int x=0; x<6; x++) {
        ret=spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
        assert(ret==ESP_OK);
        //We could inspect rtrans now if we received any info back. The LCD is treated as write-only, though.
    }
}



//Calculate the pixel data for a set of lines (with implied line size of 320). Pixels go in dest, line is the Y-coordinate of the
//first line to be calculated, linect is the amount of lines to calculate. Frame increases by one every time the entire image
//is displayed; this is used to go to the next frame of animation.
static void pretty_effect_calc_lines(uint16_t *dest, int line, int frame, int linect)
{
	uint16_t val=0;
    for (int y=line; y<line+linect; y++) {
        for (int x=0; x<320; x++) {
			uint16_t froffs=frame&0x1ff;
			val= (x+froffs)*(x+froffs)+(y+froffs)*(y+froffs);
            *dest++ = val  ;
        }
    }
}

// hash to see if we need to update the segment
static uint32_t zx_vid_hash[240*10];
static uint32_t init_cnt=2*240/PARALLEL_LINES; // make sure first update is forced




static void get_colours_from_nv()
{
    esp_err_t err;
    nvs_handle my_handle;
    ESP_ERROR_CHECK( nvs_open("zxstorage", NVS_READWRITE, &my_handle) );
    // Read
    err = nvs_get_u16(my_handle, "LCD_COL_FG", &fg_colour);
    if (err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND){
        ESP_ERROR_CHECK( err );
    }
    err = nvs_get_u16(my_handle, "LCD_COL_BG", &bg_colour);
    if (err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND){
        ESP_ERROR_CHECK( err );
    }
    nvs_close(my_handle);
}

static void store_colours_in_nv()
{
    nvs_handle my_handle;
    if(1) {
        ESP_ERROR_CHECK( nvs_open("zxstorage", NVS_READWRITE, &my_handle) );
        ESP_ERROR_CHECK( nvs_set_u16(my_handle, "LCD_COL_FG", fg_colour ) );
        ESP_ERROR_CHECK( nvs_set_u16(my_handle, "LCD_COL_BG", bg_colour ) );
        ESP_ERROR_CHECK( nvs_commit(my_handle) ); 
        nvs_close(my_handle);
    }
}



void lcd_set_colour_cmd(char cmd, uint16_t data)
{
    uint16_t prev_fg=fg_colour;
    uint16_t prev_bg=bg_colour;
    if(cmd=='N') {
        bg_colour=actual_colour;
        fg_colour=BLACK;
    }
    if(cmd=='I'){
        fg_colour=actual_colour;
        bg_colour=BLACK;
    }
    if(cmd=='W'){
        actual_colour=WHITE;
    }
    if(cmd=='G'){
        actual_colour=0x081F;
    }
    if(cmd=='A'){
        actual_colour=0x03FF;
    }
    if(bg_colour==BLACK) fg_colour=actual_colour;
    else bg_colour=actual_colour;

    if(fg_colour!=prev_fg || bg_colour!=prev_bg){
        /* force update */
        init_cnt=240/PARALLEL_LINES;
        store_colours_in_nv();
    }
}


static uint8_t *attr_mem_fg=NULL;
static uint8_t *attr_mem_bg=NULL;

static uint16_t trans_colour(uint8_t rgb_colour ){
    uint16_t v=0;
    if(rgb_colour&VIDATTR_RED) v|= RED;
    if(rgb_colour&VIDATTR_GREEN) v|= GREEN;
    if(rgb_colour&VIDATTR_BLUE) v|= BLUE;
    return v;
}


static int32_t zx_attr_hash[30];  // attribute hash per line

static bool zx_calc_lines(uint16_t *dest, int line, int frame, int linect)
{
	bool update=false;
	uint16_t fg,bg,val=0;
	uint32_t m;
	int ix;
	/* first check if we need an update */
	if(init_cnt) {update=true; init_cnt--; }
	for (int y=line; y<line+linect; y++) {
		if (update) break;
        if((y&7)==0){
            int aix=(40*(y/8));
            int32_t hash=0;
            for(int i=0;i<40;i++) { hash+=attr_mem_fg[aix]-attr_mem_bg[aix]; aix++;  }
            if(hash!=zx_attr_hash[y/8]) {update=true; break;};
        }
		for (int xw=0; xw<10; xw++) {
			if (update) break;
			ix=10*y+xw;
			if(zx_vid_hash[ix] != vid_pixel_mem[ix]){
				update=true;
			}
		}
	}
	if(!update) return false;
	/* do the actual update */
	for (int y=line; y<line+linect; y++) {
        int aix=(40*(y/8));
		for (int xw=0; xw<10; xw++) {
            int32_t attrhash=0;
			ix=10*y+xw;
			m=zx_vid_hash[ix]=vid_pixel_mem[ix];
			for (int c=0; c<4; c++) {
                attrhash+=attr_mem_fg[aix]-attr_mem_bg[aix];
                fg=trans_colour(attr_mem_fg[aix]);
                bg=trans_colour(attr_mem_bg[aix]);
                for (int b=0; b<8; b++) {
                    val= (m & 0x80000000) ? fg:bg ;
                    *dest++ = val  ;
                    m<<=1;
                }
                aix++;
            }
            zx_attr_hash[y/8]=attrhash;
		}
	}
	return true;
}

static uint32_t num_upd=0;

//Simple routine to generate some patterns and send them to the LCD. Don't expect anything too
//impressive. Because the SPI driver handles transactions in the background, we can calculate the next line
//while the previous one is being sent.
static void display_pretty_colors(spi_device_handle_t spi)
{
    uint16_t *lines[2];
    //Allocate memory for the pixel buffers
    for (int i=0; i<2; i++) {
        lines[i]=heap_caps_malloc(320*PARALLEL_LINES*sizeof(uint16_t), MALLOC_CAP_DMA);
        assert(lines[i]!=NULL);
    }
    int frame=0;
    //Indexes of the line currently being sent to the LCD and the line we're calculating.
    int sending_line=-1;
    int calc_line=0;

    while(1) {
        frame++;
        //for (int y=0; y<240; y+=PARALLEL_LINES) {
		for (int y=240-PARALLEL_LINES; y>=0; y-=PARALLEL_LINES) { /* reverse order to more  quickly change lower (edit) screen */
			bool have_new_data=true;
            //Calculate a line.
			if (frame>3)
				have_new_data=zx_calc_lines(lines[calc_line], y, frame, PARALLEL_LINES);
			else
            	pretty_effect_calc_lines(lines[calc_line], y, frame, PARALLEL_LINES);
            
			if(have_new_data){
				//Finish up the sending process of the previous line, if any
				if (sending_line!=-1) send_line_finish(spi);
				//Swap sending_line and calc_line
				sending_line=calc_line;
				calc_line=(calc_line==1)?0:1;
				//Send the line we currently calculated.
				send_lines(spi, y, lines[sending_line]);
				num_upd++;
			}
            vTaskDelay(1);  // keep system responsive TOD make this dynamic only if VGA out is active or so...

            //The line set is queued up for sending now; the actual sending happens in the
            //background. We can go on to calculate the next line set as long as we do not
            //touch line[sending_line]; the SPI sending process is still reading from that.
        }
		//if(frame%200==199){ ESP_LOGI(TAG,"Upd/200frames %d \n",num_upd);num_upd=0; }
		//vTaskDelay(12 / portTICK_RATE_MS);
        vTaskDelay(1);
    }
}

void lcd_disp_init()
{
    vidattr_get_mem(&attr_mem_fg, &attr_mem_bg);    
    get_colours_from_nv();
    if(fg_colour==BLACK) actual_colour=bg_colour;
    else actual_colour=fg_colour;

    xTaskCreate(tftd_task, "SPI_Display", 1024 * 3, NULL, 9, NULL);
}

static void tftd_task(void*arg)
{
    ESP_LOGI(TAG,"tftd_task START \n");
    esp_err_t ret;
    spi_device_handle_t spi;
    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=PARALLEL_LINES*320*2+8
    };
    spi_device_interface_config_t devcfg={
#ifdef CONFIG_LCD_OVERCLOCK
        .clock_speed_hz=26*1000*1000,           //Clock out at 26 MHz
#else
        .clock_speed_hz=10*1000*1000,           //Clock out at 10 MHz
#endif
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               //CS pin
        .queue_size=7,                          //We want to be able to queue 7 transactions at a time
        .pre_cb=lcd_spi_pre_transfer_callback,  //Specify pre-transfer callback to handle D/C line
    };
    //Initialize the SPI bus
    ret=spi_bus_initialize(VSPI_HOST, &buscfg, 1);
    ESP_ERROR_CHECK(ret);
    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(VSPI_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);
    //Initialize the LCD
    lcd_init(spi);
    //Initialize the effect displayed
    //ret=pretty_effect_init();
    //ESP_ERROR_CHECK(ret);

    //Go do nice stuff.
    display_pretty_colors(spi);

}
