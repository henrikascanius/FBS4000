#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "fbs_log.h"

// #include "fbs_gpio.h"

// ****************************
// ***** FBS GPIO INPUTS: *****
// ****************************

// Input, serial output from DSA shift register
#define GP_SRDATA_BANK      0
#define GP_SRDATA_BIT       20

// Input, Write Data from DRC
#define GP_WRDATA_BANK      1   // SP0(0)
#define GP_WRDATA_BIT       12

// Input, Write Enable from DRC
#define GP_WE_BANK          1
#define GP_WE_BIT           14

// Input, CP_DSP, clock pulse incrementing Drum Segment Address in DRC
#define GP_CPDSA_BANK       2
#define GP_CPDSA_BIT        16

// Input, latched G_i_First_Segment - First Segment was written to DRC by RC4000
#define GP_SRRQ_BANK        2
#define GP_SRRQ_BIT         17


// *****************************
// ***** FBS GPIO OUTPUTS: *****
// *****************************

// Output, clock to DSA shift register
#define GP_SRCLK_BANK       0
#define GP_SRCLK_BIT        26

// Output, Connected status to DRC
#define GP_CONN_BANK        1
#define GP_CONN_BIT         29

// Output, drum index to DRC
#define GP_INDEX_BANK       2
#define GP_INDEX_BIT        1

// Output, Read Data to DRC
#define GP_RDDATA_BANK      2
#define GP_RDDATA_BIT       3

// Output, drum clock to DRC
#define GP_RDCLK_BANK       2
#define GP_RDCLK_BIT        4

// LED outputs:

#define GP_LED0_BANK        3
#define GP_LED0_BIT         19

#define GP_LED1_BANK        3
#define GP_LED1_BIT         21

#define GP_LED2_BANK        2
#define GP_LED2_BIT         24

#define GP_LED3_BANK        2
#define GP_LED3_BIT         25

#define GP_LED4_BANK        2
#define GP_LED4_BIT         13

#define GP_LED5_BANK        2
#define GP_LED5_BIT         11

#define GP_LED6_BANK        1
#define GP_LED6_BIT         28

#define GP_LED7_BANK        2
#define GP_LED7_BIT         8

int led_banks[] = {GP_LED0_BANK, GP_LED1_BANK, GP_LED2_BANK, GP_LED3_BANK,
                   GP_LED4_BANK, GP_LED5_BANK, GP_LED6_BANK, GP_LED7_BANK};
                   
int led_bits[] =  {GP_LED0_BIT, GP_LED1_BIT, GP_LED2_BIT, GP_LED3_BIT,
                   GP_LED4_BIT, GP_LED5_BIT, GP_LED6_BIT, GP_LED7_BIT};                    

#define MAX_GPIO_BANKS              (4)

// GPIO register size
#define AM335X_GPIO_SIZE		    0x1000

#define AM335X_GPIO_OE		    	0x134
#define AM335X_GPIO_DATAIN			0x138
#define AM335X_GPIO_DATAOUT			0x13C
#define AM335X_GPIO_SETDATAOUT		0x190
#define AM335X_GPIO_CLEARDATAOUT	0x194

#define OUT          (0)
#define IN           (1)

#define CONFIG_MODULE_BASE          0x44E10000

uint32_t gpio_base[MAX_GPIO_BANKS] = {
	0x44E07000,
	0x4804C000,
	0x481AC000,
	0x481AE000
};

void *gpio_addr[MAX_GPIO_BANKS] = { NULL };

uint32_t *gpio_oe_addr[MAX_GPIO_BANKS]                  = { NULL };
volatile uint32_t *gpio_dataout_addr[MAX_GPIO_BANKS]    = { NULL };
volatile uint32_t *gpio_datain_addr[MAX_GPIO_BANKS]     = { NULL };
uint32_t *gpio_setdataout_addr[MAX_GPIO_BANKS]          = { NULL };
uint32_t *gpio_cleardataout_addr[MAX_GPIO_BANKS]        = { NULL };

uint32_t gpio_mirror[MAX_GPIO_BANKS];

#define INVMASK  0x66666600
#define MAXUNITS 4

int unit_fd[MAXUNITS];
uint32_t *img[MAXUNITS];
uint32_t unit_segs[MAXUNITS];
int seek_error = 0;
int dirty[4];

int unit_to_led[MAXUNITS] = {0, 1, 2, 3}; 

int selected_unit = -1;
uint32_t dsa = 0;  // Drum Segment Address

// Track buffer, 4*268 24-bit words
uint32_t trbuf[4*268];

void abend(char *s)
{
    FBS_LOG(G_ERROR, "ABEND: %s", s); 
    fprintf(stderr, "ABEND: %s\n", s);
    exit(1);
}

void gpio_set_direction(int bank, int pin, int direction)
{
	uint32_t reg;
	reg = *gpio_oe_addr[bank];
	if (direction == OUT) {
		reg = reg & ~(1 << pin);
	} else {
		reg = reg | (1 << pin);
	}
    *gpio_oe_addr[bank] = reg;
}

void gpio_set(int bank, int pin)
{   // Not faster than write the whole mirror...
	*gpio_setdataout_addr[bank] = (1 << pin);
	gpio_mirror[bank] |= (1 << pin);
}

void gpio_clear(int bank, int pin)
{
	*gpio_cleardataout_addr[bank] = (1 << pin);
	gpio_mirror[bank] &= (~(1 << pin));
}

uint32_t gpio_read_bank(int bank)
{
    return *gpio_datain_addr[bank];
}

int gpio_read_pin(int bank, int pin)
{
    return *gpio_datain_addr[bank] & (1<<pin) ? 1:0;
}


void gpio_write_bank_from_mirror(int bank)
{
    *gpio_dataout_addr[bank] = gpio_mirror[bank];
}

void set_led(int led, int val)
{   // LED is on when GPIO out is low
    if (val)
        gpio_mirror[led_banks[led]] &= ~(1 << led_bits[led]);
    else
        gpio_mirror[led_banks[led]] |= 1 << led_bits[led];
        gpio_write_bank_from_mirror(led_banks[led]);
}

void gpio_init()
{
    int mem_fd;
	int bank = 0, i;
	uint32_t *config;
	
	/* Setup pinmux for pins that are not GPIO already */
	
	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
		fprintf(stderr, "main: can't open /dev/mem \n");
		exit(-1);
	}
	config = mmap(  NULL,                   //Any adddress in our space will do
                    0x2000,                 //Map length
                    PROT_READ|PROT_WRITE,   // Enable reading & writting to mapped memory
                    MAP_SHARED,             //Shared with other processes
                    mem_fd,                 //File to map
                    CONFIG_MODULE_BASE      //Offset to GPIO peripheral
                  );
    
    if (config[0x8E8>>2] != 0x00000027 ||
        config[0x8EC>>2] != 0x00000027 ||
        config[0x8BC>>2] != 0x00000027 ||
        config[0x8B4>>2] != 0x00000027 ||
        config[0x8A8>>2] != 0x00000027)
    {
        fprintf(stderr, "PINMUX settings detected: \n");
        fprintf(stderr, "GPIO88: %08X\n",config[0x8E8>>2]);
        fprintf(stderr, "GPIO89: %08X\n",config[0x8EC>>2]);
        fprintf(stderr, "GPIO77: %08X\n",config[0x8BC>>2]);
        fprintf(stderr, "GPIO75: %08X\n",config[0x8B4>>2]);
        fprintf(stderr, "GPIO72: %08X\n",config[0x8A8>>2]);
        fprintf(stderr, "One or more required GPIO pins have incorrect pinmux settings\n");
        fprintf(stderr, "Please include disable_uboot_overlay_video=1 in /boot/uEnv.txt\n");
        exit(1);
    }

    /* map access to GPIO banks */
    
	for (bank=0; bank<MAX_GPIO_BANKS; bank++)
	{
        gpio_addr[bank] = mmap( NULL,                   //Any adddress in our space will do
                                AM335X_GPIO_SIZE,       //Map length
                                PROT_READ|PROT_WRITE,   // Enable reading & writting to mapped memory
                                MAP_SHARED,             //Shared with other processes
                                mem_fd,                 //File to map
                                gpio_base[bank]         //Offset to GPIO peripheral
                              );
        
        gpio_oe_addr[bank] = gpio_addr[bank] + AM335X_GPIO_OE;             
        gpio_datain_addr[bank] = gpio_addr[bank] + AM335X_GPIO_DATAIN;             
        gpio_dataout_addr[bank] = gpio_addr[bank] + AM335X_GPIO_DATAOUT;             
        gpio_setdataout_addr[bank] = gpio_addr[bank] + AM335X_GPIO_SETDATAOUT;             
        gpio_cleardataout_addr[bank] = gpio_addr[bank] + AM335X_GPIO_CLEARDATAOUT;             
    } 
    
	close(mem_fd); //No need to keep mem_fd open after mmap
	for (bank = 0; bank < MAX_GPIO_BANKS; bank++) {
		if (gpio_addr[bank] == MAP_FAILED) {
			fprintf(stderr, "mmap error %d\n", (int)gpio_addr[bank]);//errno also set!
			exit(-1);
		}
	}
    
	gpio_set_direction(GP_SRDATA_BANK, GP_SRDATA_BIT, IN);
	gpio_set_direction(GP_WRDATA_BANK, GP_WRDATA_BIT, IN);
	gpio_set_direction(GP_WE_BANK, GP_WE_BIT, IN);
	gpio_set_direction(GP_CPDSA_BANK, GP_CPDSA_BIT, IN);
	gpio_set_direction(GP_SRRQ_BANK, GP_SRRQ_BIT, IN);

    gpio_set_direction(GP_SRCLK_BANK, GP_SRCLK_BIT, OUT);
    gpio_set_direction(GP_CONN_BANK, GP_CONN_BIT, OUT);
    gpio_set_direction(GP_INDEX_BANK, GP_INDEX_BIT, OUT);
    gpio_set_direction(GP_RDDATA_BANK, GP_RDDATA_BIT, OUT);
    gpio_set_direction(GP_RDCLK_BANK, GP_RDCLK_BIT, OUT);

	for (i=0; i<8; i++)
	    gpio_set_direction(led_banks[i], led_bits[i], OUT);
	
	for (bank = 0; bank < MAX_GPIO_BANKS; bank++)
		gpio_mirror[bank] = *gpio_dataout_addr[bank];
		
    gpio_clear(GP_SRCLK_BANK, GP_SRCLK_BIT);
    gpio_clear(GP_CONN_BANK, GP_CONN_BIT);
    gpio_clear(GP_INDEX_BANK, GP_INDEX_BIT);
    gpio_clear(GP_RDDATA_BANK, GP_RDDATA_BIT);
    gpio_clear(GP_RDCLK_BANK, GP_RDCLK_BIT);
} // gpio_init

void file_init()
{
    char uname[5];
    char *fname;
    struct stat sb;
    int units = 0;
    
    strcpy(uname,"UNIT");
    uname[0] = 0;
    
    for (int unit=0; unit<MAXUNITS; unit++)
    {
        uname[4] = unit + '0';
        fname = getenv(uname);
        if (fname)
        {
            if ((unit_fd[unit] = open(uname, O_RDWR|O_SYNC)) < 0)
            {
                fprintf(stderr, "File not found: %s\n", fname);
                exit(1);
            }
            if (fstat(unit_fd[unit], &sb)== -1)
                abend("fstat");
            if (sb.st_size < 768*4) // At least one track...
                abend("filesize");
            img[unit] = mmap(NULL, sb.st_size, PROT_READ|PROT_WRITE,
                             MAP_PRIVATE | MAP_POPULATE,
                             unit_fd[unit], 0);
            if (img[unit] == MAP_FAILED)
                abend("mmap");
            unit_segs = sb.st_size / 768;
            units++;
        }
        else
        {
            unit_fd[unit] = -1;
            img[unit] = NULL;
            unit_segs[unit] = 0;
        }
    }
    if (!units)
        abend("No disk units");
} // file_init


int poll_dsa(uint32_t *reg)
{
    int i;
    uint32_t sr = 0;
    
    if (!gpio_read_pin(GP_SRRQ_BANK, GP_SRRQ_BIT))
        return 0;
    
    // SRCLK is inverted by IC4C
    for (i=0; i<19; i++)
    {
        gpio_set(GP_SRCLK_BANK, GP_SRCLK_BIT); // CP 1->0
        sr = (sr >> 1) | (gpio_read_pin(GP_SRDATA_BANK, GP_SRDATA_BIT) << 18);
        gpio_clear(GP_SRCLK_BANK, GP_SRCLK_BIT); // CP 0->1
    }
    *reg = sr;
    return 1;
}

void fetch_track()
{
    // Build track image from file data
    uint32_t track = dsa >> 2;
    uint32_t *imgptr;
    int tridx = 0;
    uint32_t parity = 0;
    
    seek_error = (track << 2) > max_segs[selected_unit];
    if (!seek_error)
    {
        imgptr = img[selected_unit] + track*768; // (768 b / 4b/w) * 4 seg/tr
        for (int sect=0; sect<4; sect++)
        {
            // We keep the word numbering of the DRC...
            // Sector data occupies word 0..255. Reformat to 24-bit
            //    24-bit:     32-bit (file):
            //      cba0            dcba                           
            //      fed0            hgfe
            //      ihg0            lkji
            //      lkj0
            for int i=0; i<256/4; i++)
            {
                parity ^= (trbuf[tridx++] = (imgptr[0] << 8) ^ INVMASK);
                parity ^= (trbuf[tridx++] = ((((imgptr[0] & 0xff000000) >> 16) | (imgptr[1] << 16))) ^INVMASK);
                parity ^= (trbuf[tridx++] = ((((imgptr[1] & 0xffff0000) >> 8)  | (imgptr[2] << 24))) ^INVMASK);
                parity ^= (trbuf[tridx++] = (imgptr[2] & 0xffffff00) ^INVMASK);
                imgptr += 3;
            }
            trbuf[tridx++] = parity;
            for int i=0; i<11; i++)
            {
                trbuf[tridx++] = (((track << 2) + sect)<< 8) | 0x80000000; See DRC018
            }
        }
        bzero(dirty, sizeof(dirty));
    }
}

void flush_track()
{
    // Update dirty sectors in file data
    uint32_t track = dsa >> 2;
    uint32_t *imgptr;
    int tridx;

    for (int sect=0; sect<4; sect++)
    {
        if (dirty[sect])
        {
            imgptr = img[selected_unit] + track*768 + sect*(768/4);
            tridx = sect*268;
            for int i=0; i<256/4; i++)
            {
                (imgptr++)* = (track[tridx] >> 8) | ((track[tridx+1] & 0x0000ff00) << 16);
                (imgptr++)* = (track[tridx+1] >> 16) | ((track[tridx+2] & 0x00ffff00) << 8);
                (imgptr++)* = (track[tridx+2] >> 24) | (track[tridx+3] & 0xffffff00);
                tridx += 4;
            }
        }
    }
}
            

void select_unit(int unit);
{
    if (selected_unit >= 0)
        set_led(unit_to_led[selected_unit], 0);
    selected_unit = unit;
    fetch_track();
    set_led(unit_to_led[unit], 1);
    FBS_LOG(G_SEEK, "Unit sel: %d", unit);
}
        
        
int main (int argc, char *argv[])
{
	int i = 0, j = 0;
    int tw;
    int rw;
    int dummy;
 
    fbs_openlog();
	gpio_init();
	file_init();
	
	
	FBS_LOG(G_MISC, "Started");

	// Short LED test at startup
    for (j=0; j<8; j++)
    {
        set_led(j,1);
        usleep(100000);
        set_led(j,0);
        usleep(100000);
    }

    
	for (i=0; i<1000000; i++) {
        tw = 0x5555aaaa;
        for (j=0; j<24; j++) {
            // gpio_set(2, 3);
            gpio_mirror[2] |= (1<<3);
	        gpio_write_bank_from_mirror(2);
	        
            rw = (rw << 1) | gpio_read_pin(2, 1);
            if (tw & 1)
    	        gpio_mirror[2] = (gpio_mirror[2] & ~(1 << 3)) | (1 << 2);
            else
    	        gpio_mirror[2] = (gpio_mirror[2] & ~(1 << 3)) & ~(1 << 2);
	        gpio_write_bank_from_mirror(2);
            tw >>= 1;
            dummy = gpio_read_pin(2, 1);
        }
	}
}