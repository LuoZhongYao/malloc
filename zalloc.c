#define TEST
#ifdef  TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
static jmp_buf jmp;
static volatile int running = 0;
#define panic(fmt,...)  ({printf(fmt,##__VA_ARGS__);running = 0;longjmp(jmp,1);})
#else
#define panic(fmt,...)
#endif
#include "list.h"
#include <stdint.h>
#define LBYTE(x)        ((x) << 2)
#define LWORD(x)        ((x) >> 2)
#define LLG2(x)         ((x) - LHEAD)
#define LHEAD           LWORD(sizeof(uint32_t))
#define LMIN            LWORD(sizeof(MBlock))
#define NR_HEAP         2
#define L1MAX           (26 * 1024)
#define L2MAX           (16 * 1024)
#define PTR_ENTRY(ptr)  container_of(ptr,MBlock,ptr)
#define LIST_ENTRY(lst) list_entry(lst,MBlock,list)
#define BLOCK_DOWN(x)   ((void*)(x) + LBYTE((x)->length))
#define BLOCK_UP(x)     ((void*)(x) - LBYTE((x)->adjlength))

#define M_ALLOC_LG2     15

#define STATISTICS

#define is_power_of_2(x)    (!((x) & (x -1 )))
#define pow2(x) (1 << x)

const uint16_t CONST_HEAP[NR_HEAP] = {L1MAX,L2MAX};

typedef struct{
    uint32_t used:1;
    uint32_t length:15;
    uint32_t :1;
    uint32_t adjlength:15;
    union{
        struct list_head list;
        char ptr[0];
    };
}MBlock;

struct{
    void     *heap[NR_HEAP];
    uint16_t hLength[NR_HEAP];
    struct list_head head[M_ALLOC_LG2];
#ifdef  STATISTICS
    int32_t scnt[M_ALLOC_LG2];
    int32_t sunhit[M_ALLOC_LG2];
#endif
}thiz;

static inline int fixsize(uint16_t size){
    if(is_power_of_2(size)) return size;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
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

#ifdef  TEST
static void freemap(void){
    struct list_head *pos;
    MBlock *block;
    for(int i = 0;i < M_ALLOC_LG2;i++){
        if(!list_empty(&thiz.head[i])){
            printf("<%4d %2d> => ",pow2(i),i);
            list_for_each(pos,&thiz.head[i]){
                block = LIST_ENTRY(pos);
                printf("{%4d,%4d} -> ",block->length,block->adjlength);
                if(!block->length)
                    panic("\nBug> Invald block\n");
            }
            printf("{}\n");
        }
    }
}
#endif

static inline MBlock* lblock(MBlock *block,uint16_t length){
    MBlock *down;
    block->length = length;
    down = BLOCK_DOWN(block);
    down->adjlength = length;
    return down;
}

static inline int limit(MBlock *block){
    for(int i = 0;i < NR_HEAP;i++){
        if(block == thiz.heap[i])
            return i;
    }
    return -1;
}

static inline MBlock *mergedown(MBlock *block){
    MBlock  *down;
    down = BLOCK_DOWN(block);
    while(-1 == limit(down) && !down->used){     /*! 堆上的内存不在空闲表里,不参与此次合并 !*/
        list_del(&down->list);
        down = lblock(block,block->length + down->length);
    }
    return block;
}

static inline MBlock *mergeup(MBlock *block){
    MBlock *up;
    up = BLOCK_UP(block);
    while(!up->used){
        list_del(&up->list);
        lblock(up,up->length + block->length);
        block = up;
        up = BLOCK_UP(block);
    }
    return block;
}

static inline MBlock *merge(MBlock *block){
    return mergedown(mergeup(block));
}

static void insert(MBlock *block){
    int bit;
    uint16_t len,blen;
    int i = 0;
    MBlock *down;
    if(block->length < LHEAD)
        panic("Bug> Insert invalid block\n");
    down = BLOCK_DOWN(block);
    if(-1 != (i = limit(down))){
        thiz.heap[i] = block;
        thiz.hLength[i] += block->length;
    }else{
        bit = bitindex(LLG2(block->length)) - 1;
        if(bit > M_ALLOC_LG2)
            panic("Bug> Beyond the scpoe of management,only support 0 ~ %d,current is %d,bit is %d\n",pow2(M_ALLOC_LG2),block->length,bit);
        blen = pow2(bit) + LHEAD;
        len = block->length - blen;
        if(len >= LMIN){
            down = lblock(block,blen);
            lblock(down,len);
            insert(down);
        }
        block->used = 0;
        list_add(&block->list,thiz.head + bit);
    }
}

void zMInit(void){
    MBlock *block;
    memset(&thiz,0,sizeof(thiz));
    for(int i = 0;i < M_ALLOC_LG2;i++){
        thiz.head[i] = (struct list_head)LIST_HEAD_INIT(thiz.head[i]);
    }
#ifdef  TEST
    for(int i = 0;i < NR_HEAP;i++){
        block = malloc(CONST_HEAP[i]);
#else
#error "You should realize allocate memory"
#endif
        block->used = 1;
        block->adjlength = 0;
        block = lblock(block,1);
        thiz.heap[i] = block;
        thiz.hLength[i] = LWORD(CONST_HEAP[i]) - 1;
    }
}

static inline MBlock *split(MBlock *block,uint16_t length){
    MBlock *down;
    const uint16_t len = block->length - length;
    if(block->length < length)
        panic("Bug > length %8d length %8d\n",length,block->length);
    if(len >= LMIN + LHEAD){
        down = lblock(block,length);
        down->length = len;
        insert(down);
    }
    return block;
}

static inline void *allocHeap(uint16_t length){
    MBlock *block;
    for(int i = 0;i < NR_HEAP;i++){
        if(thiz.hLength[i] >= length){
            block = thiz.heap[i];
            thiz.heap[i] = (void*)thiz.heap[i] + LBYTE(length);
            thiz.hLength[i] -= length;
            block->used = 1;
            lblock(block,length);
            return block->ptr;
        }
    }
    return NULL;
}

void *zMalloc(uint16_t length){
    if(!length) return NULL;
    length = length > LBYTE(LMIN) ? length : LBYTE(LMIN);
    uint16_t fix = bitindex(fixsize(length)) - 1;
    length = LWORD(length) + LHEAD;
    MBlock *block;
#ifdef  STATISTICS
    if(fix < M_ALLOC_LG2) thiz.scnt[fix]++;
#endif
    for(int i = fix;i < M_ALLOC_LG2;i++){
#ifdef  STATISTICS
        if(i != fix) thiz.sunhit[fix]++;
#endif
        if(!list_empty(&thiz.head[i])){
            struct list_head *list = thiz.head[i].next;
            list_del(list);
            block = LIST_ENTRY(list);
            block = split(block,length);
            block->used = 1;
            return block->ptr;
        }
    }
    return allocHeap(length);
}

void zFree(void *ptr){
    MBlock *block = container_of(ptr,MBlock,ptr);
    block = merge(block);
    insert(block);
}

#ifdef  TEST
#include <unistd.h>
#include <sys/signal.h>
#include <time.h>
#define PS2 (1 << 12)
static void __exit(int req){
    (void)req;
    printf("program is about to exit\n");
    running = 0;
    longjmp(jmp,1);
}
static void *ptr[PS2] = {NULL};

static void randTest(void){
    int n,nf = 0,ns = 0,nn = 0,an = 0;;
    srand(time(NULL));
    running = 1;
    setjmp(jmp);
    while(running){
        n = rand() % (PS2) & 0xFFF0;
        if(ptr[n]){
            nn++;
            zFree(ptr[n]);
            ptr[n] = NULL;
        }else{
            ptr[n] = zMalloc(n);
            if(ptr[n] == NULL){
                nf++;
            }else{
                ns++;
                an += n;
            }
        }
    }
    printf("failed %8d,success  %8d,free %8d,alloc %8d\n",nf,ns,nn,an);
}

#if 1
static void reTest(void){
    int n;
    setjmp(jmp);
    running = 1;
    while(running && scanf("%d",&n) != EOF){
        printf("alloc %d\n",n);
        if(NULL == zMalloc(n)){
            printf("Memory out\n");
            break;
        }
    }
}
#endif

int main(int argc,char **argv){
    (void)argc,(void)argv;
    signal(SIGINT,__exit);
    signal(SIGSEGV,__exit);
    zMInit();
    if(argc > 1)
        reTest();
    else
        randTest();
#ifdef  STATISTICS
    for(int i = 0;i < M_ALLOC_LG2;i++){
        printf("{%5d count %8d,unhit %8d}\n",1 << i,thiz.scnt[i],thiz.sunhit[i]);
    }
    printf("------------------------ Map -----------------------\n");
    for(int i = 0;i < NR_HEAP;i++){
        printf("<heap %d> => {%4d}\n",i,thiz.hLength[i]);
    }
    freemap();
#endif
    return 0;
}
#endif

