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

struct msg_data {
    char databuf[45];
};

typedef struct msg_data msg_data_t;

int main()
{
/** user-space application source code
  * example channel: rpmsg-openamp-demo-channel [src=0x2 ----&gt; dst=0x1]
  */

    int status;
    msg_data_t data_buf;

    struct rpmsg_endpoint_info ept_info = {"rpmsg-openamp-demo-channel", 0x29, 0x1f};
    int fd = open("/dev/rpmsg_ctrl0", O_RDWR);

    /* create endpoint interface */
    ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept_info);  // /dev/rpmsg0 is created 

    /* create endpoint */
    int fd_ept = open("/dev/rpmsg0", O_RDWR); // backend creates endpoint
    
    char test_arr [26][45];

    for (int i = 0; i < 26; i++)
    {
        for (int j = 0; j < 45; j++)
        {
            test_arr[i][j] = 'A' + i;
        }
    }

    /* send data to remote device */ 
    for (int i = 0; i < TC_TRANSFER_COUNT; i++)
    {
        status = write(fd_ept, test_arr[i], sizeof(char) * 45);
        if (status) {
            printf("the write status : %d\n", status);
        }  
    }  

    /* receive data from remote device */
    for (int i = 0; i < TC_TRANSFER_COUNT; i++)
    {    
        status = read(fd_ept, &data_buf.databuf, sizeof(data_buf.databuf));
        if (status) {
            printf("the read status : %d\n", status);
        }
        printf("%s\n",data_buf.databuf);
    }

    /* destroy endpoint */
    ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);

    close(fd_ept);
    close(fd);

    return 0;
}

