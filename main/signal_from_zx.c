#include <stdio.h>
#include <string.h>
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spi_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "iis_videosig.h"
#include "zx_server.h"
#include "signal_from_zx.h"


static const char* TAG = "sfzx";




#define USEC_TO_SAMPLES(us)   (us*20/32) 
#define MILLISEC_TO_SAMPLES(us)   (us*20000/32) 
static inline uint32_t samples_to_usec(uint32_t samples){
	return samples*32/20;
}



static bool data_vid_active=0;



static zxserv_evt_type_t phase=ZXSG_INIT;

static const char* phasenames[]={"INIT","SLOW-50Hz","SLOW-60Hz","SAVE","SILENCE","HIGH","NOISE"};

static void set_det_phase(zxserv_evt_type_t newphase)
{
	if(newphase!=phase){
		phase=newphase;
		zxsrv_send_msg_to_srv( newphase, 0,0);
		ESP_LOGI(TAG,"Detected %s \n", phasenames[newphase-ZXSG_INIT]  );
	}
}


typedef enum {
    ZXFS_INIT     = 0,        /*!< initial status */
    ZXFS_HDR_RECEIVED   = 1,        /*!< start retriving file */
    ZXFS_RETRIEVE_DATA   = 2,                 /*!< during file transfer, check if ZX loads or ignores */
} zxfs_state_t;


typedef struct zxfile_rec_status_info
{
    zxfs_state_t state;
    uint8_t pulscount;
    uint8_t bitcount;
    uint8_t data;
    uint16_t e_line;
    uint16_t bytecount;
    uint16_t namelength;
} zxfile_rec_status_t;

/* forward declarations*/
static void IRAM_ATTR samplefunc_normsave(uint32_t data);
static void IRAM_ATTR samplefunc_qsave_start(uint32_t data);

static void (*checksamplefunc)(uint32_t) = samplefunc_normsave;


static zxfile_rec_status_t zxfile;    // some signal statistics


static void zxfile_bit(uint8_t bitval)
{
    if(bitval) zxfile.data |= (0x80>>zxfile.bitcount);
    if(++zxfile.bitcount>=8) {
        // have a byte
        if(zxfile.bytecount%100<=1) ESP_LOGI(TAG,"ZXFile byte %d data %x\n",zxfile.bytecount,zxfile.data );
        if(zxfile.bytecount == zxfile.namelength+16404-16393) zxfile.e_line=zxfile.data;
        if(zxfile.bytecount == zxfile.namelength+16405-16393) {
            zxfile.e_line+=zxfile.data<<8;
            ESP_LOGI(TAG,"File E_LINE %d - len %d+%d\n",zxfile.e_line,zxfile.e_line-16393,zxfile.namelength);
        }
		zxsrv_send_msg_to_srv( ZXSG_FILE_DATA, zxfile.bytecount, zxfile.data);
		if(zxfile.bytecount==0 && zxfile.data==ZX_SAVE_TAG_QSAVE_START) {
			checksamplefunc=samplefunc_qsave_start;
		}
        zxfile.bitcount=0;
        zxfile.bytecount++;
        // zx memory image is preceided by a name that end with the first inverse char (MSB set)
        if (zxfile.namelength==0 && (zxfile.data&0x80) ) zxfile.namelength=zxfile.bytecount;
        zxfile.data=0;
        set_det_phase(ZXSG_SAVE);
    }
}

static void zxfile_check_bit_end(uint32_t duration)
{
    //if(zxfile.bytecount%50==2) printf(" ZXFile bit %d pulses (%d us) \n",zxfile.pulscount,samples_to_usec(level.duration) );
    // test have shown that the 4 and 9 pulses are retrieved quite precisely, nevertheless add some tolerance
    if(zxfile.pulscount>=3 && zxfile.pulscount<=5){
        zxfile_bit(0);
    }
    else if(zxfile.pulscount>=7 && zxfile.pulscount<=11){
        zxfile_bit(1);
    }
    else{
        ESP_LOGI(TAG,"File read retrieved %d pulses (dur %d us), cancel\n",zxfile.pulscount,samples_to_usec(duration));
        zxfile.state=ZXFS_INIT;
    }
    zxfile.pulscount=0;
}


static void analyze_1_to_0(uint32_t duration)
{
	// end of high phase
	if (zxfile.state>=ZXFS_HDR_RECEIVED){
		if(duration>=USEC_TO_SAMPLES(90) && duration<=USEC_TO_SAMPLES(250)){ // should be 150u
			++zxfile.pulscount;
		}
	}



}


static void analyze_0_to_1(uint32_t duration)
{

	if(duration>USEC_TO_SAMPLES(300) && duration<USEC_TO_SAMPLES(600))
	{
		// could be sync, but this will be handled by the video logic, so only acti if we are not already in display mode
		if(phase!=ZXSG_SLOWM_50HZ && phase!=ZXSG_SLOWM_60HZ) set_det_phase(ZXSG_NOISE);
	}

	if (zxfile.state>=ZXFS_HDR_RECEIVED){
		if(duration<USEC_TO_SAMPLES(250)){
				// okay, gap should be 150u for pules
		} else if (duration>USEC_TO_SAMPLES(1200) && duration<USEC_TO_SAMPLES(1800)){ // 1.3ms+0.15
			zxfile_check_bit_end(duration);
		}
		else
		{
			ESP_LOGI(TAG,"File gap retrieved of %d usec, cancel with %d bytes\n",samples_to_usec(duration),zxfile.bytecount);
			zxfile.state=ZXFS_INIT;
		}
	} else { /* no header yet */
		 if ( !data_vid_active )
		 {
			 // No file but also no display nor silence
			 if(duration>USEC_TO_SAMPLES(300) && duration<USEC_TO_SAMPLES(2000)) set_det_phase(ZXSG_NOISE);
		 }

	}


	if(zxfile.state<ZXFS_HDR_RECEIVED && duration>MILLISEC_TO_SAMPLES(60))
	{
		// end of long break, could be header of file
		ESP_LOGI(TAG,"Possibly file header 01   (%d ms) \n", samples_to_usec(duration)/1000);
		memset(&zxfile,0,sizeof(zxfile_rec_status_t));
		zxfile.state=ZXFS_HDR_RECEIVED;
	}
}

static uint32_t data_num_short_0=0;


static bool actual_logic_level=0;
static uint32_t level_cnt=0;
static uint32_t holdoff_cnt=0;
static uint32_t acc_holdoff_cnt=0;



/* report if we have a regular video signal or are in FAST/LOAD/SAVE/ect */
void sfzx_report_video_signal_status(bool vid_is_active){
	if(data_vid_active!=vid_is_active){
		level_cnt=holdoff_cnt=0;
		actual_logic_level=0;
		zxfile.state=ZXFS_INIT;
		ESP_LOGI(TAG,"Video signal status: %s\n", vid_is_active?"active":"inactive" );
		if(vid_is_active) set_det_phase(ZXSG_SLOWM_50HZ);
	}
	data_vid_active=vid_is_active;
} 


/* every incoming 32-bit sample as long as vid_is_active=false */
static void IRAM_ATTR samplefunc_normsave(uint32_t data)
{
	if (data==0) {
		if(actual_logic_level){
			holdoff_cnt++;
			if(holdoff_cnt>USEC_TO_SAMPLES(14)){
				analyze_1_to_0(level_cnt);
				actual_logic_level=false;
				acc_holdoff_cnt=0;
				level_cnt=0;
			}
		} else {
			level_cnt++;
			acc_holdoff_cnt+=holdoff_cnt;
			holdoff_cnt=0;
		}
	} else if (data==0xffffffff){
		if(!actual_logic_level){
			holdoff_cnt++;
			if(holdoff_cnt>USEC_TO_SAMPLES(14)){
				analyze_0_to_1(level_cnt);
				actual_logic_level=true;
				acc_holdoff_cnt=0;
				level_cnt=0;
			}
		} else {
			level_cnt++;
			acc_holdoff_cnt+=holdoff_cnt;
			holdoff_cnt=0;
		}
	} 
}

static IRAM_ATTR void check_sample_func( void(*samplefunc)(uint32_t)  );


static void IRAM_ATTR samplefunc_waitemptyline(uint32_t data);
static void IRAM_ATTR samplefunc_waithsync(uint32_t data);


static void IRAM_ATTR samplefunc_nop(uint32_t data)
{

}



static void exit_qsave_failure(uint32_t diag_data)
{
	ESP_LOGW(TAG,"Exit_from qsave with failure %d\n",diag_data);	
	for(uint32_t i=0; i<45; i++){
		ESP_LOGI(TAG,"Next sample_Data 0x%08X",vid_get_next_data());	
	}

	// release
	checksamplefunc=samplefunc_normsave;
}

static bool wait_sync()
{
	if(vid_get_next_data()==0) return false;	// missed the point
	for(uint32_t i=0; i<USEC_TO_SAMPLES(20); i++ ){
		if(vid_get_next_data()==0) return true;
	}
	return false;	// timeout
}

static void smp_delay(uint32_t samples)
{
	for(uint32_t i=0; i<samples; i++){
		vid_get_next_data();
	}
}


static uint8_t samplefunc_qsave_sample_nibble(uint32_t triggerpos, uint32_t* errorflag)
{
	uint8_t d=0;
	if(!wait_sync()) *errorflag=10;
	uint32_t p1=triggerpos-1-USEC_TO_SAMPLES(4);
	uint32_t p2=p1+USEC_TO_SAMPLES(9.58);
	uint32_t p3=p1+USEC_TO_SAMPLES(9.58*2);
	uint32_t p4=p1+USEC_TO_SAMPLES(9.58*3);
	//ESP_LOGI(TAG,"Timing= %d %d %d %d",p1,p2,p3,p4);

	smp_delay(p1);
	if (vid_get_next_data()!=0) d|=8;
	smp_delay(p2-p1-1);
	if (vid_get_next_data()!=0) d|=4;
	smp_delay(p3-p2-1);
	if (vid_get_next_data()!=0) d|=2;
	smp_delay(p4-p3-1);
	if (vid_get_next_data()!=0) d|=1;
	if (p4+1 < USEC_TO_SAMPLES(61) ) smp_delay( USEC_TO_SAMPLES(61) - p4 - 1);
	return d;
}

static void samplefunc_qsave_start(uint32_t data)
{
	ESP_LOGI(TAG,"Hello from samplefunc_qsave_start\n");	
	uint32_t errflag=0;
	uint32_t qs_count=0;
	uint32_t to_count=0;
	uint32_t line_count=0;
	uint32_t trigger_pos=0;
	//checksamplefunc=samplefunc_normsave;
	checksamplefunc=samplefunc_nop;	/* turn off to avoid recursion*/
	
	smp_delay(USEC_TO_SAMPLES(10000)); /* defay till sender is really in qsave mode */

	// wait for some empty lines
	to_count=0;
	for(;;){
		if(vid_get_next_data()==0){
		 	qs_count=0;
			if(++to_count > MILLISEC_TO_SAMPLES(250)) return exit_qsave_failure(to_count);
		}else{
			if(++qs_count > USEC_TO_SAMPLES(50)){
				// found empty line
				if(!wait_sync()) return exit_qsave_failure(1);
				if(++line_count>5) break;
			}
		}
	}
	// we are at the start of a sync now! 
	//wait for a trigger
	to_count=0;
	for(;;){
		smp_delay(USEC_TO_SAMPLES(16)); //  Wait for trigger end
		if (vid_get_next_data()==0) return exit_qsave_failure(200000+to_count);
		for(uint32_t i=USEC_TO_SAMPLES(16)+1; i<USEC_TO_SAMPLES(32); i++){
			if (vid_get_next_data()==0){
				trigger_pos=i;
				goto trigger_found;
			}
		}
		if(++to_count > 15625) return exit_qsave_failure(3); // 1 second
		smp_delay(USEC_TO_SAMPLES(24)); //  8 us left
		if(!wait_sync()) return exit_qsave_failure(300);
	}
trigger_found:
	ESP_LOGI(TAG,"QS Trigger pos %d",trigger_pos);	
	// we have a header, here we go
	// Trigger to first samplepos is 60us, choose a bit less due to delay
	smp_delay(USEC_TO_SAMPLES(56)-trigger_pos);	// end of line

	uint8_t packettype= (samplefunc_qsave_sample_nibble(trigger_pos, &errflag)<<4) | samplefunc_qsave_sample_nibble(trigger_pos, &errflag);
	uint8_t size= (samplefunc_qsave_sample_nibble(trigger_pos, &errflag)<<4) | samplefunc_qsave_sample_nibble(trigger_pos, &errflag);
	ESP_LOGI(TAG,"QS Packet %x size %d\n",packettype,size);	
	for(;;){
		uint8_t data= (samplefunc_qsave_sample_nibble(trigger_pos, &errflag)<<4) | samplefunc_qsave_sample_nibble(trigger_pos, &errflag);
		ESP_LOGI(TAG,"QS Data %x\n",data);	
		if(--size==0) break;
	}
	// release
	checksamplefunc=samplefunc_normsave;
}




/* every incoming 32-bit sample as long as vid_is_active=false */
void IRAM_ATTR sfzx_checksample(uint32_t data)
{
	(*checksamplefunc)(data);
}


/* called periodically at roughly millisec scale */
void sfzx_periodic_check(){
	if(level_cnt > MILLISEC_TO_SAMPLES(80)){
		if(actual_logic_level){
			//ESP_LOGI(TAG,"sfzx_periodic_check 1: %d %d %d\n",data_num_1,data_num_short_0,data_num_0 );
			if(acc_holdoff_cnt > level_cnt/32)	/* we see the H sync pulses but no V sync */	
				set_det_phase(ZXSG_HIGH); /* more than two frames high !*/
			else {
				/* silence, normally constant-low but due to hi pass filter is seen for us as constant high with no hsync */
				set_det_phase(ZXSG_SILENCE); 
			}
		} else {
			//ESP_LOGI(TAG,"sfzx_periodic_check 0: %d %d %d\n",data_num_1,data_num_short_0,data_num_1 );
			/* silence, normally  constant-low would not be visible as low - 
			due to high pass filter, but if for any reason we see it nevertheles, we can also react accordingly */
			set_det_phase(ZXSG_SILENCE); 
		}
	}
}          


void sfzx_init()
{}

