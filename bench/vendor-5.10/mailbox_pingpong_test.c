#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rpmsg.h"
#include <unistd.h>

#define TC_TRANSFER_COUNT 26

typedef struct the_message
{
    unsigned int DATA;
} THE_MESSAGE, *THE_MESSAGE_PTR;

int main()
{
/** user-space application source code
  * example channel: rpmsg-openamp-demo-channel [src=0x2 ----&gt; dst=0x1]
  */

    int status;

    struct rpmsg_endpoint_info ept_info = {"rpmsg-openamp-demo-channel", 0x1e, 0x1e};
    int fd = open("/dev/rpmsg_ctrl0", O_RDWR);

    /* create endpoint interface */
    ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept_info);  // /dev/rpmsg0 is created 

    /* create endpoint */
    int fd_ept = open("/dev/rpmsg0", O_RDWR); // backend creates endpoint
    
    THE_MESSAGE r5_data = {.DATA = 0};
    
    /* send data to remote device */ 
    status = write(fd_ept, &r5_data, sizeof(THE_MESSAGE));
    if (status) {
        printf("the write status : %d\n", status);
    }  

    /* receive data from remote device */
    for (int i = 0; i < TC_TRANSFER_COUNT; i++)
    {    
        status = read(fd_ept, &r5_data, sizeof(THE_MESSAGE));
        printf("r5_data.DATA: %d\n",r5_data.DATA);
        status = write(fd_ept, &r5_data, sizeof(THE_MESSAGE));
    }


    /* destroy endpoint */
    ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);

    close(fd_ept);
    close(fd);

    return 0;
}