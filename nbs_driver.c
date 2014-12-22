/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Michael Brumlow
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nbs_driver.h"

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/kernel.h>

struct gendisk *disk = NULL; 
struct block_device *blkdev = NULL; 
struct request_queue *queue = NULL; 
struct task_struct *network_task = NULL;
atomic_t pending_io;
wait_queue_head_t io_wait;
struct bio_list bio_list = {NULL, NULL};
spinlock_t bio_list_lock; 
struct completion wait_done; 

static struct block_device_operations nbs_bd_op = {
    .owner      = THIS_MODULE,
};

void 
nbs_make_request(struct request_queue *q, struct bio *bio)
{
    unsigned long flags; 

    spin_lock_irqsave(&bio_list_lock, flags);
    bio_list_add(&bio_list, bio);
    atomic_inc(&pending_io);
    wake_up(&io_wait);
    spin_unlock_irqrestore(&bio_list_lock, flags); 
}

int
send_block_info(struct socket *sock, struct page *page, struct bio *bio, unsigned char cmd)
{
    int ret = 0;
    int size = 0;
    char *buf = NULL;
    mm_segment_t oldfs;
    struct iovec iov;
    struct msghdr msg;

    buf = kmap(page);
    
    memcpy(buf, &cmd, sizeof(cmd));
    size += sizeof(cmd);
    memcpy(buf + size, &bio->bi_sector, sizeof(bio->bi_sector));
    size += sizeof(bio->bi_sector); 
    memcpy(buf + size, &bio->bi_size, sizeof(bio->bi_size));
    size += sizeof(bio->bi_size);
       
    iov.iov_base = buf; 
    iov.iov_len = size; 

    msg.msg_control=NULL;
    msg.msg_controllen=0;
    msg.msg_flags=MSG_WAITALL;
    msg.msg_name=0;
    msg.msg_namelen=0;
    msg.msg_iov=&iov;
    msg.msg_iovlen=1;

    oldfs=get_fs();
    set_fs(KERNEL_DS);
    ret=sock_sendmsg(sock,&msg,size);
    set_fs(oldfs);
    kunmap(page);
    
    if(ret != size) {
        printk(KERN_ERR "error sending blockinfo: %d\n", ret);
        return 0; 
    }
    
    return 1;
}

void 
process_io(struct socket *sock, struct page *page, struct bio *bio, char cmd)
{
    int i = 0;
    int ret = 0; 
    mm_segment_t oldfs;
    struct iovec iov;
    struct msghdr msg;
    struct bio_vec *bvec = NULL;

    if(!send_block_info(sock, page, bio, cmd)) {
         bio_endio(bio, -EIO); 
         return;
    }
    
    __bio_for_each_segment(bvec, bio, i, 0){
            
        iov.iov_base= kmap(bvec->bv_page) + bvec->bv_offset; 
        iov.iov_len = bvec->bv_len; 

        msg.msg_control=NULL;
        msg.msg_controllen=0;
        msg.msg_flags=MSG_WAITALL;
        msg.msg_name=0;
        msg.msg_namelen=0;
        msg.msg_iov=&iov;
        msg.msg_iovlen=1;

        oldfs=get_fs();
        set_fs(KERNEL_DS);
        if(cmd == 'r') { 
            ret=sock_recvmsg(sock,&msg,bvec->bv_len,msg.msg_flags);
        } else if ( cmd == 'w') {
            ret=sock_sendmsg(sock,&msg,bvec->bv_len);
        }
           
        set_fs(oldfs);
        kunmap(bvec->bv_page);
            
        if(ret != bvec->bv_len){
            printk(KERN_ERR "io_error[%c]: %llu + %d |  %d != %d\n", cmd, (unsigned long long) bio->bi_sector, bio->bi_size, ret, bvec->bv_len);
            bio_endio(bio, -EIO);
            return; 
        }

    }

    bio_endio(bio, 0);
}


void
process_bio(struct socket *sock, struct page *page, struct bio *bio)
{
    int rw = bio_rw(bio);
    
    if(rw == READA)
        rw = READ;
   
    if(rw == READ) {
        process_io(sock, page, bio, 'r');
    } else  {
        process_io(sock, page, bio, 'w');
    }
    
}

int 
network_thread(void *data) 
{   
    int ret = 0;
    int connected = 0;
    unsigned long flags;
    unsigned long need_to_process = 0; 
    struct socket *sock = NULL;
    struct sockaddr_in remoteaddr = {0, };
    struct bio_list local_bio_list = { NULL, NULL };
    struct bio *bio = NULL;
    struct page *page = NULL;

    page = alloc_page(GFP_KERNEL);
    if(!page) {
        printk(KERN_ERR "could not allocate page :(\n");   
    }

    ret = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock); 
    if(ret < 0) {
        printk(KERN_ERR "could not create socket :(\n"); 
        ret = -1;
        goto out_cleanup;
    }

    remoteaddr.sin_family = AF_INET;
    remoteaddr.sin_addr.s_addr = htonl(2130706433); // 127.0.0.1
    remoteaddr.sin_port = htons(1337);

    printk(KERN_INFO "attempting to connect to remote server.\n");
    
    ret = sock->ops->connect(sock,  (struct sockaddr *)&remoteaddr, sizeof(remoteaddr), 0);
    if( ret != 0 ) {
        printk(KERN_ERR "could not connect to socket :(\n");
    } else {
        connected = 1;    
    }
 
    printk(KERN_INFO "connected to remote server.\n");
    complete(&wait_done);
    
    while(1) {

        if(kthread_should_stop()) {
            if(need_to_process == 0) {
                break;
            }
        }

        ret = wait_event_interruptible_timeout(io_wait, atomic_read(&pending_io), msecs_to_jiffies(5000));
        
        // hit the time out but really don't have much to do here. 
        if(ret == 0 && atomic_read(&pending_io) == 0) {
            continue;   
        }

        // while we have the lock lets remove all of them that we have and 
        // put them into a local list. 
        spin_lock_irqsave(&bio_list_lock, flags);
        do {
            bio = bio_list_pop(&bio_list);      

            if(bio) {
                bio_list_add(&local_bio_list, bio); 
                atomic_dec(&pending_io);
                need_to_process++;
            }

        } while (bio); 
        spin_unlock_irqrestore(&bio_list_lock, flags);
        
        // process each bio now. 
        bio = bio_list_pop(&local_bio_list);
        while(bio) {
            need_to_process--;
            if(connected) { 
                process_bio(sock, page, bio);
            } else {
                bio_io_error(bio);
            }
            bio = bio_list_pop(&local_bio_list);
        }

    }
   

out_cleanup:
   
    if(sock) { 
        sock->ops->shutdown(sock, SHUT_RDWR); 
        sock_release(sock);
    }
    
   return ret; 
}

static
int
nbs_init(void) 
{
    atomic_set(&pending_io, 0);
    init_waitqueue_head(&io_wait);
    spin_lock_init(&bio_list_lock);
    init_completion(&wait_done);
    
    network_task = kthread_create(network_thread, NULL, "nbs_network");
    if( IS_ERR(network_task)) {
        printk(KERN_ERR "error create network task."); 
    } else { 
        wake_up_process(network_task);
        wait_for_completion(&wait_done);
    }
    
    disk = alloc_disk(1);
    if(!disk) {
        printk(KERN_ERR "could not allocate disk.\n");  
        return -1;    
    }

    queue = blk_alloc_queue(GFP_KERNEL);
    if(!queue) {
        printk(KERN_ERR "could not allocate queue.\n");
        return -1;    
    }

    blk_queue_make_request(queue, nbs_make_request);
    blk_set_default_limits(&queue->limits);

    disk->major = 37;
    disk->first_minor = 1; 
    disk->minors = 1;
    disk->queue = queue;
    disk->fops = &nbs_bd_op;

    // one gigabyte. 
    set_capacity(disk, 2097152);
    sprintf(disk->disk_name, "nbs");
    add_disk(disk);

    return 0; 
}

static
void
nbs_exit(void)
{

    if(disk) {
        del_gendisk(disk);
        put_disk(disk);
        disk = NULL;
    }

    if(queue) {
        blk_cleanup_queue(queue);
        queue = NULL;
    }

    if(network_task) {
        kthread_stop(network_task);
    }

}

MODULE_AUTHOR("Michael Brumlow");
MODULE_DESCRIPTION("NBS Example");
MODULE_LICENSE("MIT");
MODULE_VERSION("0.01"); 
module_init(nbs_init);
module_exit(nbs_exit);


