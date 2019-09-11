#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

// #include "fbs_gpio.h"

// define FBS GPIOs

// Input, latched G_i_First_Segment - First Segment was written to DRC by RC4000
#define GP_SRRQ_BANK        2
#define GP_SRRQ_BIT         17

// Output, clock to DSA shift register
#define GP_SRCLK_BANK       0
#define GP_SRCLK_BIT        26

// Input, serial output from DSA shift register
#define GP_SRDATA_BANK      0
#define GP_SRDATA_BIT       20

// Input, CP_DSP, clock pulse incrementing Drum Segment Address in DRC
#define GP_CPDSA_BANK       2
#define GP_CPDSA_BIT        16

// Output, drum clock to DRC
#define GP_RDCLK_BANK       2
#define GP_RDCLK_BIT        4

// Output, drum index to DRC
#define GP_INDEX_BANK       2
#define GP_INDEX_BIT        1

// Output, Read Data to DRC
#define GP_RDDATA_BANK      2
#define GP_RDDATA_BIT       3

// Input, Write Data from DRC
#define GP_WRDATA_BANK      1   // SP0(0)
#define GP_WRDATA_BIT       12

// Input, Write Enable from DRC
#define GP_WE_BANK          1
#define GP_WE_BIT           14

// Output, Connected status to DRC
#define GP_CONN_BANK        1
#define GP_CONN_BIT         29

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

unsigned int *gpio_oe_addr[MAX_GPIO_BANKS]                  = { NULL };
volatile unsigned int *gpio_dataout_addr[MAX_GPIO_BANKS]    = { NULL };
volatile unsigned int *gpio_datain_addr[MAX_GPIO_BANKS]     = { NULL };
unsigned int *gpio_setdataout_addr[MAX_GPIO_BANKS]          = { NULL };
unsigned int *gpio_cleardataout_addr[MAX_GPIO_BANKS]        = { NULL };

unsigned int gpio_mirror[MAX_GPIO_BANKS];

void gpio_set_direction(int bank, int pin, int direction)
{
	unsigned int reg;
	reg = *gpio_oe_addr[bank];
	if (direction == OUT) {
		reg = reg & ~(1 << pin);
	} else {
		reg = reg | (1 << pin);
	}
    *gpio_oe_addr[bank] = reg;
}

void gpio_set(int bank, int pin)
{
	*gpio_setdataout_addr[bank] = (1 << pin);
	gpio_mirror[bank] |= (1 << pin);
}

void gpio_clear(int bank, int pin)
{
	*gpio_cleardataout_addr[bank] = (1 << pin);
	gpio_mirror[bank] &= (~(1 << pin));
}

unsigned int gpio_read_bank(int bank)
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
{
    if (val)
        gpio_mirror[led_banks[led]] |= 1 << led_bits[led];
    else
        gpio_mirror[led_banks[led]] &= ~(1 << led_bits[led]);
        gpio_write_bank_from_mirror(led_banks[led]);
}

void gpio_init()
{
    int  mem_fd;
	int bank = 0;
	int *config;
	
	/* Setup pinmux for pins that are not GPIO already */
	
	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
		printf("main: can't open /dev/mem \n");
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
        printf("PINMUX settings detected: \n");
        printf("GPIO88: %08X\n",config[0x8E8>>2]);
        printf("GPIO89: %08X\n",config[0x8EC>>2]);
        printf("GPIO77: %08X\n",config[0x8BC>>2]);
        printf("GPIO75: %08X\n",config[0x8B4>>2]);
        printf("GPIO72: %08X\n",config[0x8A8>>2]);
        printf("One or more required GPIO pins have incorrect pinmux settings\n");
        printf("Please include disable_uboot_overlay_video=1 in /boot/uEnv.txt\n");
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
	for ( bank = 0; bank < MAX_GPIO_BANKS; bank++ ) {
		if (gpio_addr[bank] == MAP_FAILED) {
			printf("mmap error %d\n", (int)gpio_addr[bank]);//errno also set!
			exit(-1);
		}
		gpio_mirror[bank] = *gpio_datain_addr[bank];
	}    
} // gpio_init


int main (int argc, char *argv[])
{
	int i = 0, j = 0;
    int tw;
    int rw;
    int dummy;
	gpio_init();
	
	gpio_set_direction(2, 3, OUT);
	gpio_set_direction(2, 2, IN);
	gpio_set_direction(2, 1, IN);
	
	for (i=0; i<8; i++)
	    gpio_set_direction(led_banks[i], led_bits[i], OUT);
	
	for (i=0; i<10; i++)
	{
	    for (j=0; j<8; j++)
	    {
	        set_led(j,1);
	        usleep(100000);
	        set_led(j,0);
	        usleep(100000);
	    }
	}
	exit(0);

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