#define TEST
#ifdef  TEST
#include <stdio.h>
#include <stdlib.h>
#define panic(fmt,...)  ({\
    fprintf(stdout,fmt,##__VA_ARGS__);\
    exit(1);})
#else
#define panic(fmt,...)
#endif
#include <stdint.h>
#include <string.h>
#include "list.h"

#define MM_BLOCK_DOUBLE_BYTE   16  /*! double word !*/
#define MM_POOL_SIZE    16
#define MM_HEAP_SIZE    512
#define MM_ALLOC_LG2    16  /*! 2^16 64KB !*/
#define MM_MAX_SIZE     (50 * 1024)

#define BLOCK_OFFSET(x,off) (((char *)x) + (off << 2))
#define BLOCK_UP(x)         (((void*)x) - ((x)->adjlength << 2))
#define BLOCK_DOWN(x)       (((void*)x) + ((x)->length << 2))
#define BLOCK_ENTRY(x)      container_of((x),MMBlock,list)

#define is_power_of_2(x)    (!((x) & (x - 1)))
#define pow2(x) (1 << (x))

#define STATISTICS
typedef struct MMBlock_  MMBlock;
struct MMBlock_{
    uint32_t used:1;             /*! 当前块是否被使用,1 使用,0 空闲 !*/
    uint32_t length:15;          /*! 当前块大小的高15位,低1位永远为0,单位是4byte !*/
    uint32_t :1;
    uint32_t adjlength:15;       /*! 前相邻一块内存大小 !*/
    union{
        struct list_head list;  /*! 空闲链表 !*/
        char ptr[0];            /*! ptr ~ ptr + length就是分配的可用块 !*/
    };
};

static struct{
    struct list_head head[MM_ALLOC_LG2]; /*! 空闲内存链 !*/
#ifdef  STATISTICS
    struct{
        int32_t  count[MM_ALLOC_LG2];        /*! 内存统计信息 !*/ 
        int32_t  unhit[MM_ALLOC_LG2];        /*! 未命中数 !*/
    };
#endif
}mmInfo;

static inline int fixsize(uint32_t size){
    if(is_power_of_2(size)) return size;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return size + 1;
}

static inline int bitindex(uint16_t x){
    int n = 0;
    if(x >> 8) {n += 8; x >>= 8;}
    if(x >> 4) {n += 4; x >>= 4;}
    if(x >> 2) {n += 2; x >>= 2;}
    if(x >> 1) {n += 1; x >>= 1;}
    if(x) n += 1;
    return n;
}

#ifdef  STATISTICS
static void foreach(void){
    struct list_head *pos;
    for(int i = 0;i < MM_ALLOC_LG2;i++){
        if(!list_empty(&mmInfo.head[i])){
            printf("%8d %2d> ",1 << i,i);
            list_for_each(pos,&mmInfo.head[i]){
                printf("{ %p length %4d,adjlength %4d} -> ",pos,list_entry(pos,MMBlock,list)->length,list_entry(pos,MMBlock,list)->adjlength);
                if(!list_entry(pos,MMBlock,list)->length)
                    panic("\nBug> Invald block\n");
            }
            printf("{ }\n");
        }
    }
}
#endif

static inline void update(MMBlock *block){
    MMBlock *down;
    down = BLOCK_DOWN(block);
    down->adjlength = block->length;
}

static MMBlock *mergeup(MMBlock *block){
    MMBlock *up;
    up = BLOCK_UP(block);
    while(!up->used){  /*! 合并前邻居 !*/
        list_del(&up->list);
        up->length += block->length;
        block = up;
        up = BLOCK_UP(block);
    }
    update(block);
    return block;
}

static MMBlock *mergedown(MMBlock *block){
    MMBlock *down;
    down = BLOCK_DOWN(block);
    while(!down->used){
        list_del(&down->list);
        block->length += down->length;
        down = BLOCK_DOWN(block);
    }
    update(block);
    return block;
}

static MMBlock *merge(MMBlock *block){
    block = mergeup(block);
    block = mergedown(block);
    return block;
}
static void insert(MMBlock *block){
    int bit;
    uint16_t blen = 0,len = 0;
    MMBlock *down;
    if(!block->length) 
        panic("Error> Invalid block\n");
    bit = bitindex(block->length);
    if(!bit || bit >= 15)
        panic("Error> Beyond the scope of management,only support 0 ~ 12800,current is %d,bit is %d\n",block->length,bit);
    blen = pow2((bit - 1));

    len = block->length - blen;
    if(len >= MM_BLOCK_DOUBLE_BYTE){    /*! 如果剩下的块大于 MM_BLOCK_DOUBLE_BYTE,将它插入空闲链表 !*/
        block->length = blen;
        down = BLOCK_DOWN(block);
        down->length = len;
        insert(down);
    }
    block->used = 0;
    update(block);
    list_add(&block->list,mmInfo.head + bit);
}



void zMInit(void){
    MMBlock *block,*up,*down;
    memset(&mmInfo,0,sizeof(mmInfo));
    for(int i = 0;i < MM_ALLOC_LG2;i++){
        mmInfo.head[i].next = &mmInfo.head[i];
        mmInfo.head[i].prev = &mmInfo.head[i];
        //mmInfo.head[i] = (struct list_head)LIST_HEAD_INIT(mmInfo.head[i]);
    }
#ifdef  TEST
    up = malloc(MM_MAX_SIZE);
#else
#error "You should realize allocate memory"
#endif
    block = up + 1;
    up->length = sizeof(MMBlock) >> 2;
    up->adjlength = 0;
    up->used = 1;

    block->length = (MM_MAX_SIZE - 2 * sizeof(MMBlock)) >> 2;
    block->adjlength = sizeof(MMBlock) >> 2;

    down = BLOCK_DOWN(block);
    down->used = 1;
    down->length = sizeof(MMBlock) >> 2; /*! length align 4 !*/
    update(block);
    insert(block);
}
static inline MMBlock *split(MMBlock *block,uint16_t length){
    MMBlock *down;
    const uint16_t len = block->length - length;
    if(block->length < length) {
        panic("Bug > length %8d length %8d\n",length,block->length);
    }
    if(len >= MM_BLOCK_DOUBLE_BYTE){    /*! 如果剩于块大于 MM_BLOCK_DOUBLE_BYTE 插入空闲块 !*/
        block->length = length;
        update(block);
        down = BLOCK_DOWN(block);
        down->length = len;
        insert(down);
    }
    return block;
}

void *zMalloc(uint16_t length){
    if(!length) return NULL;
    length = ((length + sizeof(int32_t)) > sizeof(MMBlock) ? (length + sizeof(int32_t)) : sizeof(MMBlock)) >> 2;
    uint16_t fix = bitindex(fixsize(length)) + 1;
    MMBlock *block;
#ifdef  STATISTICS
    if(fix < MM_ALLOC_LG2) mmInfo.count[fix]++;
#endif

    for(int i = fix;i < MM_ALLOC_LG2;i++){
#ifdef  STATISTICS
        if(i != fix) mmInfo.unhit[i]++;
#endif
        if(!list_empty(&mmInfo.head[i])){
            struct list_head *list =  mmInfo.head[i].next;
            list_del(list);
            block = BLOCK_ENTRY(list);
            block = split(block,length);
            block->used = 1;
            return block->ptr;
        }
    }
    return NULL;
}

void zFree(void *ptr){
    MMBlock *block = container_of(ptr,MMBlock,ptr);
    block = merge(block);
    insert(block);
}

#ifdef  TEST
#include <unistd.h>
#include <sys/signal.h>
#include <setjmp.h>
#include <time.h>
#define PS2 (1 << 10)
static volatile int running = 0;
static jmp_buf jmp;
static void __exit(int req){
    (void)req;
    printf("program is about to exit\n");
    running = 0;
    longjmp(jmp,1);
}
static void *ptr[PS2] = {NULL};
int main(int argc,char **argv){
    (void)argc,(void)argv;
    int n;
    int nf = 0,ns = 0,nn = 0;
    signal(SIGINT,__exit);
    signal(SIGSEGV,__exit);
    zMInit();
    srand(time(NULL));
    running = 1;
    setjmp(jmp);
    while(running){
        n = rand() % (PS2);
        if(ptr[n]){
            zFree(ptr[n]);
            ptr[n] = NULL;
            nn++;
        }else{
            ptr[n] = zMalloc(n);
            if(ptr[n] == NULL){
                nf++;
            }else{
                ns++;
            }
        }
    }
    printf("Debug> failed : %8d success : %8d free %8d\n",nf,ns,nn);
#ifdef  STATISTICS
    for(int i = 0;i < MM_ALLOC_LG2;i++){
        printf("{%5d count %d,unhit %d}\n",1 << i,mmInfo.count[i],mmInfo.unhit[i]);
    }
    printf("------------------------ Map -----------------------\n");
    foreach();
#endif
    return 0;
}
#endif