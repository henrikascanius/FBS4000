/*
 * This program enables the user to fiddle with GPIO pins.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "fbs_gpio.h"

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

/*
 * Set up a memory regions to access GPIO
 */
void gpio_init()
{
    int  mem_fd;
	int bank = 0;
	
	/* open /dev/mem */
	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
		printf("main: can't open /dev/mem \n");
		exit(-1);
	}
	
	for (bank=0; bank<MAX_GPIO_BANKS; bank++)
	{
        gpio_addr[bank]
            = mmap(
                    NULL,                   //Any adddress in our space will do
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
