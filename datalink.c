#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "datalink.h"

#define inc(k) if (k < MAX_SEQ) k++; else k = 0
#define DATA_TIMER  5000
#define ACK_TIMER 300
#define MAX_SEQ 31                      /* 帧的最大序号 */
#define NR_BUFS ((MAX_SEQ + 1) / 2)     /* 滑动窗口大小 */

typedef unsigned char seq_nr;
typedef unsigned char frame_kind;
typedef enum{false, true} bool;

/* 报文结构体*/
typedef struct {
    unsigned char info[PKT_LEN];
}packet;

/* 帧结构体 */
typedef struct FRAME{ 
    unsigned char kind;
    seq_nr ack;
    seq_nr seq;
    packet data;
    unsigned int  padding;
}Frame;

bool no_nak = true;                 /* 初始阶段默认没有nak到来 */
static int phl_ready = 0;           /* 物理层状态，初始化为未准备好 */


/* 判断 b 是否在 a 和 c 的逻辑中间 */
static bool between(seq_nr a, seq_nr b, seq_nr c)
{
    return ((a <= b && b < c) || (c < a && a <= b) || (b < c && c < a));
}

/* 向物理层发送帧 */
static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len); /* 在原始数据后面加上校验码 */
    send_frame(frame, len + 4);
    phl_ready = 0;
}

/* 发送一帧的准备工作以及最终发送 */
static void send_data(frame_kind fk, seq_nr frame_nr, seq_nr frame_expected, packet buffer[])
{
    Frame s;
    s.kind = fk;
    s.seq = frame_nr;
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
   
    if (fk == FRAME_DATA) {
        memcpy(s.data.info, buffer[frame_nr % NR_BUFS].info, PKT_LEN);  /* 从缓存读出数据到帧s */
        dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short*)&(s.data).info);
        put_frame((unsigned char *)&s, 3 + PKT_LEN);                    /* 额外信息占3字节 */
        start_timer(frame_nr % NR_BUFS, DATA_TIMER);                    /* 启动数据帧超时定时器 */
    }
    if (fk == FRAME_NAK) {
        no_nak = false;
        put_frame((unsigned char *)&s, 2);
    }
    if (fk == FRAME_ACK) {
        dbg_frame("Send ACK %d\n", s.ack);
        put_frame((unsigned char *)&s, 2);
    }
    stop_ack_timer();           /* 此时有数据发出可捎带信息，没必要再单独发送ack */
}


int main(int argc, char **argv)
{
    seq_nr next_frame_to_send;  /* 发送窗口上界 + 1 */
	seq_nr ack_expected;        /* 发送窗口下届 */
	seq_nr frame_expected;      /* 接收窗口下届 */
	seq_nr too_far;             /* 接收窗口上界 + 1 */
	int arg;                    /* 获取超时的定时器编号 */
    int len = 0;                /* 获取收到的帧的长度 */
	int i;                      /* 缓存池buffer[]的索引 */

	Frame r;                    
	seq_nr nbuffered;           /* 已发送但没确认的数据帧的数量 */
	packet out_buf[NR_BUFS];    /* 发送的分组流 */
	packet in_buf[NR_BUFS];     /* 接收的分组流 */
	bool arrived[NR_BUFS];      /* 是否接收到序号为x的帧 */
	int event;                  /* 事件类型 */

	enable_network_layer();     /* 网络层开始工作 */

    /* 一系列初始化 */
	ack_expected = 0;           
	next_frame_to_send = 0;     
	frame_expected = 0;         
	too_far = NR_BUFS;
	nbuffered = 0;               
	for(i = 0; i < NR_BUFS; i++) {
		arrived[i] = false;
	}
	protocol_init(argc,argv);
	lprintf("Designed by T-bag and RYR and ZXY. Build: " __DATE__"  "__TIME__"\n");

    /* 进入无限循环 */
    while(true) {
        event = wait_for_event(&arg);
        switch(event) {
           
            /* 从网络层获取报文并封装成帧发送 */
            case NETWORK_LAYER_READY:
                nbuffered++;    
                get_packet(out_buf[next_frame_to_send % NR_BUFS].info);
                                /* 从网络层获取分组 */
                send_data(FRAME_DATA, next_frame_to_send, frame_expected, out_buf);
                                /* 包装并发送帧 */
                inc(next_frame_to_send);
                                /* 发送窗口上界移动 */
                break;
            
            /* 物理层可以发送 */
            case PHYSICAL_LAYER_READY:
                phl_ready = 1;
                break;
           
            /* 收到某一帧 */
            case FRAME_RECEIVED:
                len = recv_frame((unsigned char *)&r, sizeof r);
                
                /* 检查是否正确传输 */
                if (len < 5 || crc32((unsigned char *)&r, len) != 0) {
                    dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                    if (no_nak)
                        send_data(FRAME_NAK, 0, frame_expected, out_buf); 
                    break;
                }

                /* 未受损的帧到达 */
                if (r.kind == FRAME_DATA) {
                    /* 序号不对且还未发送过nak */
                    if ((r.seq != frame_expected) && no_nak)
                        send_data(FRAME_NAK, 0, frame_expected, out_buf); 
                    else
                        start_ack_timer(ACK_TIMER);         /* 正确接收后启动ack计时器 */

                    /* 检查是否在窗口中 */
                    if (between(frame_expected, r.seq, too_far) && arrived[r.seq % NR_BUFS] == false) {
                        dbg_frame("Recv DATA %d %d, ID %d\n", r.seq, r.ack, *(short*)&(r.data).info);
                        arrived[r.seq % NR_BUFS] = true;
                        in_buf[r.seq % NR_BUFS] = r.data;   /* 从帧中取出数据部分 */

                        /* 数据发送给网络层并滑动窗口 */
                        while (arrived[frame_expected % NR_BUFS]) {
                            put_packet(in_buf[frame_expected % NR_BUFS].info, len - 7);
                            no_nak = true;
                            arrived[frame_expected % NR_BUFS] = false;
                            inc(frame_expected);            /* 接收窗口滑动 */
                            inc(too_far);                   /* 接收窗口滑动 */
                            start_ack_timer(ACK_TIMER);     /* 是否需要单独发送ack */
                        }
                    }
                }

                /* 到达的帧是nak，需重新发送之前的帧 */
                if (r.kind == FRAME_NAK && between(ack_expected, (r.ack + 1) % (MAX_SEQ + 1), 
                next_frame_to_send)) {
                    //dbg_frame("Recv NAK DATA %d\n", r.ack);
					send_data(FRAME_DATA, (r.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);
                }

                /* 确认收到帧后，处理nbuffered的数量*/
                while (between(ack_expected, r.ack, next_frame_to_send)) {
                    nbuffered--;
                    stop_timer(ack_expected % NR_BUFS); /* 帧完好到达，停止计时器 */
                    inc(ack_expected);                  /* 更新发送窗口下界 */
                }
                break;
            
            /* 数据帧计时器超时 */
            case DATA_TIMEOUT:
                dbg_event("******** DATA %d timeout **********\n", arg); 
				/* 找到其真正的序号 */
                if(! between(ack_expected, arg, next_frame_to_send))
					arg = arg + NR_BUFS;
				send_data(FRAME_DATA, arg, frame_expected, out_buf); 
				break;
            
            /* ack计时器超时，不再等待直接发送ack */
            case ACK_TIMEOUT:
                dbg_event("******** ACK %d timeout *********\n", arg);
				send_data(FRAME_ACK, 0, frame_expected, out_buf);
                break;
        }
    /* 判断网络层是否可以继续发送数据 */
    if (nbuffered < NR_BUFS && phl_ready)
        enable_network_layer();
    else
        disable_network_layer();
    }
}