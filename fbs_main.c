#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "fbs_gpio.h"

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

	for (i=0; i<1000000; i++) {
        tw = 0x5555aaaa;
        for (j=0; j<24; j++) {
            //gpio_set(2, 3);
            gpio_mirror[2] |= (1<<3);
	        gpio_write_bank_from_mirror(2);
	        
            rw = (rw << 1) | gpio_read_pin(2, 1);
            if (tw & 1)
    	        gpio_mirror[2] = (gpio_mirror[2] & ~(1 << 3)) | (1 << 2);
            else
    	        gpio_mirror[2] = (gpio_mirror[2] & ~(1 << 3)) & ~(1 << 2);
	        gpio_write_bank_from_mirror(2);
            tw >>= 1;
            //dummy = gpio_read_pin(2, 1);
            dummy = *gpio_datain_addr[2] & (1<<1) ? 1:0;
        }
	}
}