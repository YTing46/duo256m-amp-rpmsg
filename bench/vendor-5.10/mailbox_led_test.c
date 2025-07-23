#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rpmsg.h"
#include <unistd.h>

typedef struct the_message
{
    unsigned int DATA;
} THE_MESSAGE, *THE_MESSAGE_PTR;

int main()
{
/** user-space application source code
  * example channel: rpmsg-openamp-demo-channel [src=0x2 ----&gt; dst=0x1]
  */

    struct rpmsg_endpoint_info ept_info = {"rpmsg-openamp-demo-channel", 0x20, 0x20};
    int fd = open("/dev/rpmsg_ctrl0", O_RDWR);

    /* create endpoint interface */
    ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept_info);  // /dev/rpmsg0 is created 

    /* create endpoint */
        /* create endpoint */
    int fd_ept = open("/dev/rpmsg0", O_RDWR); // backend creates endpoint
    if (fd_ept < 0) {
        fd_ept = open("/dev/rpmsg1", O_RDWR);
    }
    
    THE_MESSAGE r5_data = {.DATA = 0};

    /* receive data from remote device */
    for (;;)
    {    
        /* send data to remote device */
        scanf("%d",&r5_data.DATA);
        if (!r5_data.DATA)
            break;
        write(fd_ept, &r5_data, sizeof(THE_MESSAGE));
        printf("write period: %d\n",r5_data.DATA);
        /* receive data from remote device */
        read(fd_ept, &r5_data, sizeof(THE_MESSAGE));
        printf("read period: %d\n",r5_data.DATA);
    }

    /* destroy endpoint */
    ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);

    close(fd_ept);
    close(fd);

    return 0;
}