
#include<unistd.h>
#include<fcntl.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<error.h>
#include<sys/ioctl.h>
#include<linux/types.h>
#include<sys/mman.h>
#include<scsi/scsi_ioctl.h>
#include<scsi/sg.h>
#include<time.h>
#include<pthread.h>

//#define DISPLAY_IDFY_HEX /*defining will display IDFY hex data*/
#define DISPLAY_IDFY_DETAILS
 /*shows IDFY human readable information*/
//#define DEBUG 
/*will printf some success and other debug information*/
//#define MEMORY_MAP
/*memory mapping is used in mix-unlimited read thread code; it gives almost 15% less CPU utilization as compared to without memory mapping*/
/*use memory map upto block size 64 there after code fails*/

#define HDD  /*defining HDD will allow to test 3 different Active-Range[upper-middle-lower]; code works for SSD as well*/

#define BUFFER_MUL 2 /*dobling the buffer size*/
#define CLOCK CLOCK_REALTIME
#define IDENTIFY_REPLY_LEN 512
#define BLOCK_MAX 65535 /*SCSI provides 16 bits for "number of sectors" to be read in one go*/
#define THREADS_MAX 64 /*number of threads can be anythig: if it is less than QD than number of threads is the new QD*/
#define CMD_LEN 16/*16 bytes SCSI command is being used here*/
#define BUFFER_SIZE 16*1024*1024*BUFFER_MUL/*buffer size is 16MiB */
#define SAMPLE_SEC 10 /*it is the granularity in which Throughput calculation is done*/
#define SECTOR_SIZE 512 /*It is Logical sector/block size*/
#define RANDOM 1
#define SEQ 2
#define MIX_UNLIMITED 1 
#define MIX_LIMITED 2
#define ALIGNED 1
#define UNALIGNED 2
#define MIX_ALIGNMENT 3 /*mix-alignment does not bother about LBA value: one of the most gruelling and unforgiving test*/

#ifndef SG_FLAG_MMAP_IO
#define SG_FLAG_MMAP_IO 4
#endif

int** data_buffer;/*this holds the 2D array of data buffer from where we take our data to write*/
/*data_buffer is prepare before testing starts depending upon Entropy and Block-size; it has been done to minimize the delay incurred by rand()*/
/*It is quite slow too fill the buffer after every IO, using rand()*/
__u64 io;/*it is incremented after every IO by every thread*/
__u64 read_io;/*it is incremented by read-threads*/
__u64 write_io;/*it is incremented by write-threads*/
int block_size ;/*variable*/
int seq_lba ;/*this keeps the sequential LBA*/
int alignment;/*a variable to store the type of alignment(aligned;unaligned;mix-aligned) */
int alignment_t;/*vaiable to store the type and later size of alignment*/
char* file_name;/*it holds the device file name*/
__u64 lba_max ;/*to be filled by identify function*/
int end_of_test = 1,row; /*must be 1 otherwise threads will end*/
int read_per,write_per;

#ifdef HDD
int range; /*range of LBA: lower,middle,upper - suitable for HDD */
#endif

typedef struct thread_arg {
	int thread_id;/*thread id*/
}argument;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

///function prototyping
void* th_rand_write(void*);
void* th_rand_read(void*);
void* th_seq_write(void*);
void* th_seq_read(void*);
void* th_rand_mix(void*);
void* th_seq_mix(void*);
int identify();
char* fill();


int main(int argc, char* argv[]){
	int read_write = 0;/*type of test: like mix-limited or mix-unlimited*/
/*mix-limited: in this test the read and write ratio is maintained throughout the test - it is suitable when read and writes are dependent*/
/*mix-unlimited: in this read and write threads are divide as per their ratio and does not block each other - it is suitable when read and writes are independent and the tester wants to know the drive's responsiveness*/
	int random_seq = 0;/*holds the type of test: random or sequential*/
	int size = 0/*test size*/,col;/*variable hold the block transfer size- it is used in data_buffer preparation*/
	int seed = 5;/*variable to store new rand() function seed*/
	__u64 c_io =0;/* variable to hold number of IO completed in one SAMPLE_SEC*/
	double c_iops = 0;/*IOPS of c_io*/
	int temp,ret,read_th/*no of read threads*/,write_th/*no of write threads*/;
	register int loop;/*variable for looping; hence register storage class*/
	int no_of_threads,entropy;
	float iops,read_iops,write_iops;
	double latency = 0;/* keep the time*/
	double t_latency = 0;/*total duration for which test runs*/
	float throughput;
	int active_range ;/*range of LBAs to consider for test */
	__u64 required_io;/*it is calculated from size*/
	struct timespec time1,time2;//to measure the latency
	if(argc<2){
		printf("Please enter the device file name\n");
		printf("for example: %s /dev/sg< >\n",argv[0]);
		return 1;
	}else file_name = argv[1];
	
	temp = identify();
	if(temp > 0){ printf("Identify Failed\n"); return 1;}

	printf("Mix-Unlimited: In this Test threads for the reads and writes are seperate - test is suitable for responsiveness\n");
	printf("Mix-limited: In this Test read and write ration is continously maintained  - suitable when reads and writes are dependent\n");
	do{	
		printf("Press 1 for Mix-Unlimited IOs; 2 for Mix-Limited IOs: ");
		scanf("%d",&read_write);
	}while(read_write > 2 || read_write < 1);
	
	do{			
		printf("Enter the Read Percentage: ");
		scanf("%d",&read_per);

	}while(read_per > 100 || read_per < 0);
	write_per = 100 - read_per;
	
	do{
		printf("Press 1 for Random IOs; Press 2 for Sequintial IOs:  ");
		scanf("%d",&random_seq);
	}while(random_seq < 1 || random_seq > 2);

	do{
		printf("Enter the Block Size i.e. No-of-sectors (in units of 512B): ");
		scanf("%d",&block_size);
	}while(block_size < 1 || block_size > BLOCK_MAX);
	if(random_seq == RANDOM){		
		printf("Alignment is required to simulate or avoid read-modify and multiple-page reads\n");/*for seq alignment must be taken care with LBA and Block_size*/	
		do{
			printf("Press 1 for Aligned IOs; Press 2 for UnAligned IOs; 3 for Mix-Alignment: ");
			scanf("%d",&alignment);

		}while(alignment < 1|| alignment > 3);

		if(alignment == 1 || alignment == 2){
			do{
				printf("Press 1 for 4K Alignment/Un-Alignment; Press 2 for 8K; and Press 3 for 16K: ");
				scanf("%d",&alignment_t);
			}while(alignment_t < 1 || alignment_t > 3);
			switch(alignment_t){
				case 1: alignment_t = (0x4);
					break;
				case 2: alignment_t = (0x8);
					break;
				case 3: alignment_t = (0x10);
					break;
				default:printf("Alignment was something weird\n");
					return 1;

			}//switch
		}
	}//if RANDOM

	printf("Active Range is the range of LBAs to be considered for the test\n");
	
	if(random_seq == RANDOM){
#ifdef HDD
		do{
			printf("Press 1 for Lower(1/3) range;2 for Middle(1/3);3 for upper(1/3);4 to Enter Percentage: ");
			scanf("%d",&range);
			if(range == 4){
				do{
					printf("Enter Active Range(percentage): ");
					scanf("%d",&active_range);	
				}while(active_range<0||active_range>100);
			}
		}while(range < 1 || range > 4);

		if(range != 4){
			lba_max = (lba_max/3)-block_size; /* it will be used in threads for deciding LBAs */
		}else{/* block_size is subtracted to avoid the crossing of maximum LBAs */
			lba_max = (lba_max*active_range/100)-block_size;/*since range = 4; active range gets applied*/ 
		}
#endif

#ifndef HDD
		do{
			printf("Enter Active Range(percentage): ");
			scanf("%d",&active_range);	
		}while(active_range<0||active_range>100);
		lba_max = (lba_max*active_range/100)-block_size;
#endif
	}else{	
		do{
			printf("Enter start LBA: ");
			scanf("%d",&seq_lba);
		}while(seq_lba<0||seq_lba>lba_max);

		do{/*with the use of active_range and start_lba any range can be tested*/
			printf("Enter Active Range(percentage): ");
			scanf("%d",&active_range);	
		}while(active_range<0||active_range>100);
		lba_max = lba_max*active_range/100;/*block size is not sub cause rap up occurs in thread itself*/
		if(lba_max < seq_lba){
			printf("problem: start LBA should be lower than Active-Range\n");
			return 1;
		}

	}

	printf("Enter the Size of the test (in MiB): ");
	scanf("%d",&size);

	required_io = size*1024*2/block_size;
	//printf("Required IOs: %lld\n",required_io);

	do{
		printf("Enter the Number of Threads: ");
		scanf("%d",&no_of_threads);		
	}while(no_of_threads < 1 || no_of_threads > THREADS_MAX);
	
	read_th = read_per*no_of_threads/100;/*since "int" is being used here the value will be floor value(i.e lowest integer) but as reads are comparetively faster than writes a thread less for read wont screw much; also we cannot have threads in decimal we will have to favour either read or write. i am going for write since its slow */
	write_th = no_of_threads - read_th;

	argument arg[no_of_threads];
	pthread_t th[no_of_threads];
	int thread_status[no_of_threads];

	if(read_per < 100){/*performed when there are some writes*/
		row = BUFFER_SIZE/SECTOR_SIZE/block_size;/*as many as 16MiB divide by block size*/
		col = block_size*SECTOR_SIZE/sizeof(int);
		data_buffer = (int**)malloc(row*sizeof(int*));

		if(data_buffer == NULL){
			printf("could not manage space for data buffer: malloc failed\n");
			return 1;
		}
		for(loop = 0; loop< row;loop++){
			data_buffer[loop]=(int*)malloc(col*sizeof(int));
			if(data_buffer[loop] == NULL){ 
				printf("memory allocation for %d row failed\n",loop);
				return 1;
			}	
		}
	
		printf("Enter the Seed for Random data: ");
		scanf("%d",&seed);//keeping the seed variable
		srand(seed);
		do{
			printf("Enter the Entropy :");
			scanf("%d",&entropy);
		}while(entropy > 100 || entropy < 10 );
		
		printf("Preparing the Data Buffer\n");
		clock_gettime(CLOCK,&time1);
		for(int i = 0;i<row;i++){
			for(loop = 0;loop< col*entropy/100;loop++){
				data_buffer[i][loop] = rand();
			}
			for(;loop<col;loop++){
				data_buffer[i][loop] = 0;
			}
		}
		clock_gettime(CLOCK,&time2);	
		latency = time2.tv_sec - time1.tv_sec + (double)((time2.tv_nsec - time1.tv_nsec)/1000000000.0) ;
		printf("Buffer Preperation took: %f sec\n",latency);
		printf("Size of Buffer %d MiB\n",sizeof(data_buffer)/1024/1024);
	}
/*Displaying all the parameters for the Test*/	
	printf("\n\nDETAILS OF TEST :\n");
	if(read_write == 1)
 		printf("MIX-UNLIMITED\n");
	else	printf("MIX-LIMITED\n");
	printf("READ PERCENTAGE: %d	WRITE PERCENTAGE: %d\n",read_per,write_per);
	printf("DATA TRANSFER SIZE PER IO: %d bytes\n",block_size*SECTOR_SIZE);
	if(alignment == 1)
		printf("IOs will be %dK Aligned\n",alignment_t);
	else if(alignment == 2)
		printf("IOs will be %dK Un-Aligned\n",alignment_t);
	else if(alignment == 3)
		printf("Mix-Alignment\n");
	printf("Test Size: %d MiB\n	Number Of IOs: %lld\n",size,required_io);
	printf("Number of threads: %d\n",no_of_threads);
	if(read_write == MIX_LIMITED)
		printf("Read threads: %d	Write_threads: %d\n",read_th,write_th);	
#ifndef HDD
	printf("Active Range: %d percent\n",active_range);
#endif
#ifdef HDD
	if(range == 1) printf("Lower 1/3rd LBA range\n");
	else if(range == 2) printf("Middle 1/3rd LBA range\n");
	else if(range == 3) printf("Upper 1/3rd LBA range\n");
	else if(range == 4) printf("Active Range: %d percent\n",active_range);
#endif
	if(read_per<100)	
		printf("Entropy: %d\n",entropy);
	if(read_write == MIX_UNLIMITED){
		printf("read-threads: %d	write-threads: %d\n",read_th,write_th);
	}

	pthread_mutex_lock(&lock);/*to avoid IO generation untill all threads are up and running*/
/*creating threads for parllel execution and queueing*/
	if(read_write == MIX_UNLIMITED && random_seq == RANDOM){	
		for (loop = 0;loop< write_th;loop++){		
			arg[loop].thread_id = loop;
#ifdef DEBUG
			printf("Creating Write Thread:TI: %d\n",arg[loop].thread_id);
#endif			
			ret = pthread_create(&th[loop],NULL,th_rand_write,(void*)&arg[loop]);
#ifdef DEBUG
			if(ret) printf("Thread creation failed:TI: %d\n",arg[loop].thread_id);
#endif
			thread_status[loop] = ret;
		}
		for (;loop< no_of_threads;loop++){		
			arg[loop].thread_id = loop;
#ifdef DEBUG
			printf("Creating Read Thread:TI: %d\n",arg[loop].thread_id);
#endif
			ret = pthread_create(&th[loop],NULL,th_rand_read,(void*)&arg[loop]);
#ifdef DEBUG
			if(ret) printf("Thread creation failed:TI: %d\n",arg[loop].thread_id);
#endif
			thread_status[loop] = ret;
		}
	}else if(read_write == MIX_UNLIMITED && random_seq == SEQ){	
		for (loop = 0;loop< write_th;loop++){		
			arg[loop].thread_id = loop;
#ifdef DEBUG
			printf("Creating Write Thread:TI: %d\n",arg[loop].thread_id);
#endif
			ret = pthread_create(&th[loop],NULL,th_seq_write,(void*)&arg[loop]);
#ifdef DEBUG
			if(ret) printf("Thread creation failed:TI: %d\n",arg[loop].thread_id);
#endif
			thread_status[loop] = ret;
		}
		for (;loop< no_of_threads;loop++){		
			arg[loop].thread_id = loop;
#ifdef DEBUG
			printf("Creating Read Thread:TI: %d\n",arg[loop].thread_id);
#endif
			ret = pthread_create(&th[loop],NULL,th_seq_read,(void*)&arg[loop]);
#ifdef DEBUG
			if(ret) printf("thread creation failed:TI: %d\n",arg[loop].thread_id);
#endif
			thread_status[loop] = ret;
		}
	}else if (read_write == MIX_LIMITED && random_seq == RANDOM){
		
		for (loop = 0;loop< no_of_threads;loop++){		
			arg[loop].thread_id = loop;
#ifdef DEBUG
			printf("Creating Thread:TI: %d\n",arg[loop].thread_id);
#endif
			ret = pthread_create(&th[loop],NULL,th_rand_mix,(void*)&arg[loop]);
#ifdef DEBUG
			if(ret) printf("Thread creation failed:TI: %d\n",arg[loop].thread_id);
#endif
			thread_status[loop] = ret;
		}


	}else if (read_write == MIX_LIMITED && random_seq == SEQ){
		
		for (loop = 0;loop< no_of_threads;loop++){		
			arg[loop].thread_id = loop;
#ifdef DEBUG
			printf("Creating Thread:TI: %d\n",arg[loop].thread_id);
#endif
			ret = pthread_create(&th[loop],NULL,th_seq_mix,(void*)&arg[loop]);
#ifdef DEBUG
			if(ret) printf("Thread creation failed:TI: %d\n",arg[loop].thread_id);
#endif
			thread_status[loop] = ret;
		}


	}



	while(io < required_io){
		clock_gettime(CLOCK,&time1);
		pthread_mutex_unlock(&lock);/*start the IO generation*/
		sleep(SAMPLE_SEC);
		pthread_mutex_lock(&lock);
		clock_gettime(CLOCK,&time2);

		latency = time2.tv_sec - time1.tv_sec + (double)((time2.tv_nsec -time1.tv_nsec)/1000000000.0) ;
		t_latency += latency;
		iops = io/t_latency;
		c_iops = (io - c_io)/latency;
		read_iops = read_io/t_latency;
		write_iops = write_io/t_latency;
		throughput = iops*block_size/2048.0;
		//printf("********\n\nIOPS: %d\nCURRENT IOPS: %f\nREAD IOPS: %d\nWRITE IOPS: %d\nTHROUGHPUT(MiB/s): %f\n",iops,c_iops,read_iops,write_iops,throughput);
		printf("***************\nTest Completed: %0.3f %% \t Time consumed(in IOs): %lf\n",(double)io*100/required_io,t_latency);
		printf("********\nAVG IOPS: %0.3f\t\tAVG THROUGHPUT(MiB/s): %0.3f\nCURRENT IOPS: %0.3f\t\tCURRENT THROUGHPUT(MiB/s): %0.3f\nREAD IOPS: %0.3f\t\tWRITE IOPS: %0.3f\nCURRENT LATENCY: %lf ms\n",iops,throughput,c_iops,c_iops*block_size/2048.0,read_iops,write_iops,(double)latency*1000/(io-c_io));
		c_io = io;

				
	}
	end_of_test = 0;
/*joining threads after the completion of test*/
	for(int i=0;i<no_of_threads;i++){
		if(thread_status[i]) continue;
#ifdef DEBUG
		printf("waiting for thread:TI: %d to join\n",i);
#endif
		pthread_join(th[i],NULL);
	}

return 0;
}


char* fill(){/*this function, upon calling returns a row which of size SECTOR_SIZE*block_size and offset get rollovered after 16MiB/(SECTOR_SIZE*block_size) times*/
	static int offset = -1;
	offset++;
	if(offset == row){
		offset = 0;/*rollover*/
	}
	
	return (char*)data_buffer[offset];
}


void* th_rand_write(void* par){
	argument* arg = (argument*)par;
	
	unsigned char write_cdb[16] = {0x8A,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	unsigned int lba;/*since LBA is of 32 bit- this code can test 1 TB drives only*/
	int fd;/*to store the file-descriptor of device file - every thread will open its seperate device for parllelism or to avail queueing*/
	unsigned char sense_buffer[32];
	sg_io_hdr_t io_hdr;

	fd = open(file_name,O_RDWR);
	if(fd==0){
		printf("TI: %d could not open the device file\n",arg->thread_id);
		exit(1);
	}
#ifdef DEBUG
	else printf("TI: %d device file successfully opened, fd is: %d\n",arg->thread_id,fd);
#endif
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = CMD_LEN;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.dxfer_len = (SECTOR_SIZE*block_size);
	io_hdr.cmdp = write_cdb;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = 20000;

	while(end_of_test){
		
		pthread_mutex_lock(&lock);

		io_hdr.dxferp = fill();/*fill does not use mutex so must be used inside */
		io++;
		write_io++;

		pthread_mutex_unlock(&lock);
#ifdef HDD
		if(range == 2){
			lba = lba_max + rand()%lba_max;
		}else if(range == 3){
			lba = lba_max + lba_max + rand()%lba_max ;
		}else {
			lba = rand()% lba_max;/*for range =1; lba_max would be complete range divided by 3 - since lower range; for range = 4; lba_max would be "active_range percentage of complete range"*/
		}
#endif

#ifndef HDD
		lba = rand()% lba_max;/*lba_max would be percentage of active range*/
#endif
		if(alignment == ALIGNED){
			lba = lba & (~(alignment_t-1)); 
		}else if(alignment == UNALIGNED){
			lba = ((lba % alignment_t)!= 0)?lba:lba+1;
		}/*else do not care about the alignment */
		write_cdb[6] = lba>>24;
		write_cdb[7] = lba>>16;
		write_cdb[8] = lba>>8;
		write_cdb[9] = lba;
		/*using IOCTL in random IOs and will use write-read combination in SEQUENTIAL IOs since sequential IO needs asynchronous call to synchronize them using "mutex"- write-read combination can be used in random IOs as well but then that would mean two system-calls and two kernelmode-usermode translation and their consequencess*/
		if(ioctl(fd,SG_IO,&io_hdr)<0){
			printf("TI: %d ioctl failed\n",arg->thread_id);
#ifdef DEBUG
			for(int i = 0 ; i<32;i++)
				printf("%hx ",sense_buffer[i]);
#endif
			exit(1);
		}	

	}//while end

}//function end





void* th_seq_write(void* par){
	argument* arg= (argument*)par;
	
	unsigned char write_cdb[16] = {0x8A,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	int ret;
	int fd;
	unsigned char sense_buffer[32];
	sg_io_hdr_t io_hdr;

	fd = open(file_name,O_RDWR);
	if(fd==0){
		printf("TI: %d could not open the device file\n",arg->thread_id);
		exit(1);
	}
#ifdef DEBUG
	else printf("TI: %d device file successfully opened, fd is: %d\n",arg->thread_id,fd);
#endif
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = CMD_LEN;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.dxfer_len = (SECTOR_SIZE*block_size);
	io_hdr.cmdp = write_cdb;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = 20000;

	while(end_of_test){
		
		pthread_mutex_lock(&lock);
		
		io_hdr.dxferp = fill();
		io++;
		write_io++;

		seq_lba = (seq_lba+block_size)%lba_max;

		write_cdb[6] = seq_lba>>24;
		write_cdb[7] = seq_lba>>16;
		write_cdb[8] = seq_lba>>8;
		write_cdb[9] = seq_lba;

		ret = write(fd,&io_hdr,sizeof(io_hdr));

		pthread_mutex_unlock(&lock);
		if(!ret){
			printf("TI: %d write function failed\n",arg->thread_id);			exit(1);
		}
#ifdef DEBUG
		else printf("TI: %d write function was successful\n",arg->thread_id);
#endif		
		ret = read(fd,&io_hdr,sizeof(io_hdr));
		if(!ret){
			printf("TI: %d read function failed\n",arg->thread_id);
			exit(1);
		}
	}//while end

}//function end



void* th_rand_read(void* par){
	argument* arg= (argument*)par;
	
	unsigned char read_cdb[16] = {0x88,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	unsigned int lba;
	int fd;
#ifdef MEMORY_MAP
	unsigned char* buffer = NULL;	
#endif	
#ifndef MEMORY_MAP
	unsigned char buffer[SECTOR_SIZE*block_size];
#endif	
	unsigned char sense_buffer[32];
	sg_io_hdr_t io_hdr;

	fd = open(file_name,O_RDWR);
	if(fd==0){
		printf("TI: %d could not open the device file\n",arg->thread_id);
		exit(1);
	}
#ifdef DEBUG
	else printf("TI: %d device file successfully opened; fd is: %d\n",arg->thread_id,fd);
#endif

#ifdef MEMORY_MAP
	buffer = (unsigned char*)mmap(NULL, SECTOR_SIZE*block_size, PROT_READ|PROT_WRITE,MAP_SHARED, fd, 0);	
	if(buffer == NULL){
		printf("TI: %d Memory Mapping failed\n",arg->thread_id);
		exit(1);	
	}
#ifdef DEBUG
	else printf("TI: %d Memory Mapping was successful\n",arg->thread_id);
#endif
		
#endif
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = CMD_LEN;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = (SECTOR_SIZE*block_size);
#ifndef MEMORY_MAP
	io_hdr.dxferp = buffer;
#endif
	io_hdr.cmdp = read_cdb;
	io_hdr.sbp = sense_buffer;
#ifdef MEMORY_MAP
	io_hdr.flags = SG_FLAG_MMAP_IO;
#endif
	io_hdr.timeout = 20000;

	while(end_of_test){
		
		pthread_mutex_lock(&lock);
		
		io++;
		read_io++;

		pthread_mutex_unlock(&lock);

#ifdef HDD
		if(range == 2){
			lba = lba_max + rand()%lba_max;
		}else if(range == 3){
			lba = lba_max + lba_max + rand()%lba_max ;
		}else {
			lba = rand()% lba_max;
		}
#endif

#ifndef HDD
		lba = rand()% lba_max;
#endif

		if(alignment == ALIGNED){
			lba = lba & (~(alignment_t-1)); 
		}else if(alignment == UNALIGNED){
			lba = ((lba % alignment_t)!= 0)?lba:lba+1;
		}

		read_cdb[6] = lba>>24;
		read_cdb[7] = lba>>16;
		read_cdb[8] = lba>>8;
		read_cdb[9] = lba;
		
		if(ioctl(fd,SG_IO,&io_hdr)<0){
			printf("TI: %d ioctl failed\n",arg->thread_id);
#ifdef DEBUG
			for(int i = 0 ; i<32;i++)
				printf("%hx ",sense_buffer[i]);
#endif
			exit(1);
		}

	}//while

}//function end


void* th_seq_read(void* par){
	argument* arg= (argument*)par;
	
	unsigned char read_cdb[16] = {0x88,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	int ret;
#ifdef MEMORY_MAP
	unsigned char* buffer = NULL;
#endif
#ifndef MEMORY_MAP
	unsigned char buffer[SECTOR_SIZE*block_size];
#endif
	int fd;
	unsigned char sense_buffer[32];
	sg_io_hdr_t io_hdr;

	fd = open(file_name,O_RDWR);
	if(fd==0){
		printf("TI: %d could not open the device file\n",arg->thread_id);
		exit(1);
	}
#ifdef DEBUG
	else printf("TI: %d device file successfully opened, fd is: %d\n",arg->thread_id,fd);
#endif

#ifdef MEMORY_MAP
	buffer = (unsigned char*)mmap(NULL, SECTOR_SIZE*block_size, PROT_READ|PROT_WRITE,MAP_SHARED, fd, 0);	
	if(buffer == NULL){
		printf("TI: %d Memory Mapping failed\n",arg->thread_id);
		exit(1);	
	}
#ifdef DEBUG
	else printf("TI: %d Memory Mapping was successful\n",arg->thread_id);
#endif
		
#endif
	
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = CMD_LEN;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = (SECTOR_SIZE*block_size);
#ifndef MEMORY_MAP
	io_hdr.dxferp = buffer;
#endif
	io_hdr.cmdp = read_cdb;
	io_hdr.sbp = sense_buffer;
#ifdef MEMORY_MAP
	io_hdr.flags = SG_FLAG_MMAP_IO;
#endif
	io_hdr.timeout = 20000;

	while(end_of_test){
		pthread_mutex_lock(&lock);
		io++;
		read_io++;	
	
		seq_lba = (seq_lba+block_size)%lba_max;

		read_cdb[6] = seq_lba>>24;
		read_cdb[7] = seq_lba>>16;
		read_cdb[8] = seq_lba>>8;
		read_cdb[9] = seq_lba;

		ret = write(fd,&io_hdr,sizeof(io_hdr));

		pthread_mutex_unlock(&lock);
		if(!ret){
			printf("TI: %d write of read cmd failed\n",arg->thread_id);			exit(1);
		}
#ifdef DEBUG
		else printf("TI: %d write was successful\n",arg->thread_id);
#endif		
		ret = read(fd,&io_hdr,sizeof(io_hdr));
		if(!ret){
			printf("TI: %d read function for read failed\n",arg->thread_id);
			exit(1);
		}
	}//while end

}//function end


void* th_rand_mix(void* par){
	argument* arg = (argument*)par;
	
	unsigned char write_cdb[16] = {0x8A,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	unsigned char read_cdb[16] = {0x88,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	unsigned int lba;
	int fd;
	int count100 = 0,c_read=0,c_write=0;/*c_read means count for read part*/
	unsigned char buffer[SECTOR_SIZE*block_size];
	unsigned char sense_buffer[32];
	sg_io_hdr_t io_hdr;

	fd = open(file_name,O_RDWR);
	if(fd==0){
		printf("TI: %d could not open the device file\n",arg->thread_id);
		exit(1);
	}
#ifdef DEBUG
	else printf("TI: %d device file successfully opened, fd is: %d\n",arg->thread_id,fd);
#endif
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = CMD_LEN;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
//	io_hdr.dxfer_direction = ; /*initialized in respective part of code*/
	io_hdr.dxfer_len = (SECTOR_SIZE*block_size);
//	io_hdr.cmdp = ; /*initialied in respective part of code*/
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = 20000;

	while(end_of_test){
		if(count100 == 100){/*100 percent*/
			c_read = 0;
			c_write = 0;
			count100 = 0;
		}
		if(c_write < write_per){
			io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
			io_hdr.cmdp = write_cdb;


			pthread_mutex_lock(&lock);

			io_hdr.dxferp = fill();
			io++;
			write_io++;

			pthread_mutex_unlock(&lock);
#ifdef HDD
			if(range == 2){
				lba = lba_max + rand()%lba_max;
			}else if(range == 3){
				lba = lba_max + lba_max + rand()%lba_max ;
			}else {
				lba = rand()% lba_max;
			}
#endif

#ifndef HDD
			lba = rand()% lba_max;
#endif

			if(alignment == ALIGNED){
				lba = lba & (~(alignment_t-1)); 
			}else if(alignment == UNALIGNED){
				lba = ((lba % alignment_t)!= 0)?lba:lba+1;
			}
			write_cdb[6] = lba>>24;
			write_cdb[7] = lba>>16;
			write_cdb[8] = lba>>8;
			write_cdb[9] = lba;
			
			if(ioctl(fd,SG_IO,&io_hdr)<0){
				printf("TI: %d ioctl failed\n",arg->thread_id);
#ifdef DEBUG
				for(int i = 0 ; i<32;i++)
					printf("%hx ",sense_buffer[i]);
#endif
				exit(1);
			}	
		c_write++;	
		count100++;
		}//if write

		if(c_read < read_per){

			io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
			io_hdr.cmdp = read_cdb;
			io_hdr.dxferp = buffer;

			pthread_mutex_lock(&lock);
			
			io++;
			read_io++;

			pthread_mutex_unlock(&lock);
#ifdef HDD
			if(range == 2){
				lba = lba_max + rand()%lba_max;
			}else if(range == 3){
				lba = lba_max + lba_max + rand()%lba_max ;
			}else {
				lba = rand()% lba_max;
			}
#endif
	
#ifndef HDD
			lba = rand()% lba_max;
#endif

			if(alignment == ALIGNED){
				lba = lba & (~(alignment_t-1)); 
			}else if(alignment == UNALIGNED){
				lba = ((lba % alignment_t)!= 0)?lba:lba+1;
			}

			read_cdb[6] = lba>>24;
			read_cdb[7] = lba>>16;
			read_cdb[8] = lba>>8;
			read_cdb[9] = lba;
		
			if(ioctl(fd,SG_IO,&io_hdr)<0){
				printf("TI: %d ioctl failed\n",arg->thread_id);
#ifdef DEBUG
				for(int i = 0 ; i<32;i++)
					printf("%hx ",sense_buffer[i]);
#endif
				exit(1);
			}
		
		c_read++;
		count100++;
		}//if read
	}//while end

}//function end


void* th_seq_mix(void* par){
	argument* arg= (argument*)par;
	
	unsigned char write_cdb[16] = {0x8A,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	unsigned char read_cdb[16] = {0x88,0,0,0,0,0,0,0,0,0,0,0,(block_size >> 8),block_size,0,0};
	int ret;
	int fd;
	int count100 = 0,c_read=0,c_write=0;
	unsigned char buffer[SECTOR_SIZE*block_size];
	unsigned char sense_buffer[32];
	sg_io_hdr_t io_hdr;

	fd = open(file_name,O_RDWR);
	if(fd==0){
		printf("TI: %d could not open the device file\n",arg->thread_id);
		exit(1);
	}
#ifdef DEBUG
	else printf("TI: %d device file successfully opened, fd is: %d\n",arg->thread_id,fd);
#endif
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = CMD_LEN;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
//	io_hdr.dxfer_direction = ;
	io_hdr.dxfer_len = (SECTOR_SIZE*block_size);
//	io_hdr.cmdp = ;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = 20000;

	while(end_of_test){
		
		if(count100 == 100){
			c_read = 0;
			c_write = 0;
			count100 = 0;
		}
		if(c_write < write_per){
			io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
			io_hdr.cmdp = write_cdb;

			pthread_mutex_lock(&lock);
		
			io_hdr.dxferp = fill();
			io++;
			write_io++;
	
			seq_lba = (seq_lba+block_size)%lba_max;
	

			write_cdb[6] = seq_lba>>24;
			write_cdb[7] = seq_lba>>16;
			write_cdb[8] = seq_lba>>8;
			write_cdb[9] = seq_lba;

			ret = write(fd,&io_hdr,sizeof(io_hdr));

			pthread_mutex_unlock(&lock);
			if(!ret){
				printf("TI: %d write function failed\n",arg->thread_id);			
				exit(1);
			}
#ifdef DEBUG
			else printf("TI: %d write function was successful\n",arg->thread_id);
#endif		
			ret = read(fd,&io_hdr,sizeof(io_hdr));
			if(!ret){
				printf("TI: %d read function failed\n",arg->thread_id);
				exit(1);
			}
		c_write++;
		count100++;
		}//if write

		if(c_read < read_per){

			io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
			io_hdr.cmdp = read_cdb;
			io_hdr.dxferp = buffer;
			pthread_mutex_lock(&lock);
			io++;
			read_io++;	
	
			seq_lba = (seq_lba+block_size)%lba_max;
	

			read_cdb[6] = seq_lba>>24;
			read_cdb[7] = seq_lba>>16;
			read_cdb[8] = seq_lba>>8;
			read_cdb[9] = seq_lba;
	
			ret = write(fd,&io_hdr,sizeof(io_hdr));

			pthread_mutex_unlock(&lock);
			if(!ret){
				printf("TI: %d write of read cmd failedd\n",arg->thread_id);		
				exit(1);
			}
#ifdef DEBUG
			else printf("TI: %d write was successful\n",arg->thread_id);
#endif		
			ret = read(fd,&io_hdr,sizeof(io_hdr));
			if(!ret){
				printf("TI: %d read function for read failedd\n",arg->thread_id);
				exit(1);
			}
		c_read++;
		count100++;
		}//if read
	}//while end

}//function end




int identify(){
	int fd,i;
	unsigned char identify_cdb[12] = 
		{0xa1,0x0c,0x0d,1,1,0,0,0,0,0xec,0,0};

	sg_io_hdr_t io_hdr;
	__u16 buffer[IDENTIFY_REPLY_LEN];/*since identify give data in wordsize*/
	unsigned char sense_buffer[32];
	
	/////////opening the device file/////////////

	if((fd = open(file_name,O_RDWR))<0){
		printf("Device file Opening failed in Identify command\n");
		return  1;
	}
	

	/*data buffer initialization*/
	for(i=0 ; i< (IDENTIFY_REPLY_LEN) ; i++){
		buffer[i] = 0;
	}
	printf("\n");
		
	////////////////prepare read//////
	memset(&io_hdr,0,sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = sizeof(identify_cdb);
	io_hdr.mx_sb_len = sizeof(sense_buffer);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = (IDENTIFY_REPLY_LEN);
	io_hdr.dxferp = buffer;
	io_hdr.cmdp = identify_cdb;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = 20000;


	if(ioctl(fd,SG_IO,&io_hdr)<0){
#ifdef DEBUG
		printf("IOCTL failed in IDFY function\nsense data:\n");
		for(i = 0 ; i<32;i++)
			printf("%hx ",sense_buffer[i]);
		return 1;
#endif
	}
	close(fd);
	
#ifdef DISPLAY_IDFY_HEX	
	printf("********data buffer after ioctl***********\n");
	for(i=0; i<(IDENTIFY_REPLY_LEN) ; i++){
		printf("%hx ",buffer[i]);
	}
#endif
	printf("\n");
#ifdef DISPLAY_IDFY_DETAILS
	printf("SATA IDENTIFY DETAILS\n");	
	if(buffer[75]&0x001f)
		printf("Queue Depth supported\nQueue Depth: %d\n",(int)(buffer[75]&0x001f)+1);/*+1 since queue depth reported is 1 less than actual value*/

	if(buffer[85]&0x0020)	
		printf("Volatile Write Cache is enabled\n");
	
	if(buffer[85]&0x2000)	
		printf("Read Buffer command is supported\n");
	
	if(buffer[85]&0x1000)	
		printf("Write Buffer Command is supported\n");
	
	if(buffer[86]&0x0400)	
		printf("48 bit address features set is supported\n");
	
	if(buffer[86]&0x2000)	
		printf("Flush Cache Ext command is supported\n");

	if(buffer[86]&0x1000)	
		printf("Flush Cache command is supported\n");
	
	if(buffer[69]&0x0008){
		printf("Extenden number of user addressable LBAs: %lld\n", *((__u64*)(buffer+230)));
	}	
	
	//printf("Logical Sector Size: %d\n", *((__u32*)(buffer+117)));
	printf("User Addressable LBAs[word 60]: %d\n", *((__u32*)(buffer+60)));
	printf("User Addressable LBAs[word 100]: %lld\n", *((__u64*)(buffer+100)));

	if((buffer[106]&0xc000) == 0x4000)
	{
		if(buffer[106]&0x2000) printf("Device has multiple Logical Sectors per Physical Sectors\n");
		if(buffer[106]&0x1000) printf("Device Logical Sector longer than 512B\n");
			else printf("Logical Sector size is 512B\n");
		printf("Logical Sectors per Physical Sector (2^): %d\n",(int)(buffer[106]&0x000f));
	}
#endif
	lba_max =  *((__u64*)(buffer+100));
return 0;
}


