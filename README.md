# SATA-PERFORMANCE-UTILITY
This is an Utility to measure the performance of SATA Drives. It can test both HDD and SSD.

This Utility measures the following parameters:
1. IOPS: Input/output operations per second
   – CURRENT IOPS: Average IOPS in 10 seconds duration
   – AVERAGE IOPS: Average of CURRENT IOPS for the complete duration of the test.
2. THROUGHPUT: Bandwidth of the data transfer – unit: MiB/s
– CURRENT THROUGHPUT and AVERAGE THROUGHPUT.
3. LATENCY: Response Time – unit: ms

This Utility considers the following Test Parameters:
1. Random and Sequential IOs
2. Variable Block Sizes
3. Read/Write Ratio
4. Queue Depth
5. Active Range
6. IOs Alignment and UN-alignment
7. The entropy of the data written.

    1. Random and Sequential IOs: The utility can issue both Random IOs and 100 % Sequential IOs. The Random IO means LBAs of neighbouring operations would not be related in any ways. To generate Random LBAs I am using  Rand() function available in Linux OSes. The Rand() function are suitable for repeatability of the test and compare the performance of two drives. The Sequential IO means LBA of next IO will be “present LBA + block_size.” For submitting Random IOs, the utility uses the IOCTL call. IOCTL call is a synchronous call i.e. it returns only when the IO is complete. For submitting sequential IOs, the utility uses the combination of WRITE and READ calls. WRITE call is used to submit the IO command and READ call to poll its completion. It (the use of WRITE and READ calls) was necessary for sequential IOs as that needs synchronisation of parallel threads.
    
    2. variable Block Size: Block size means the number of sectors (LBA) to be considered around the passed LBA for data transfer.
   
    3. Read/Write Ratio: Utility can handle any read/write ratio from 100% read to 100% write. The utility uses two different methods of parallelism owing to read/write test and difference in reading and writing speed – reading from a drive is always faster than writing to a drive.
    a. Mix-Unlimited Test: In this case, threads for reading and writing are separate. Consequentially, reading continues even when writing threads are stuck due to write slowness. Thus, Mix-Unlimited Test allows us to test the responsiveness of the drive.
    b. Mix-Limited Test: In this case, reading and writing happen from same thread and read/write ratio is strictly maintained. This test is useful when reading and writing are interdependent.
    
    4. Queue Depth: Queue-Depth is defined as the number of parallel commands a drive can handle. SATA drives have queue-depth of 32. one can control the queue-depth either by editing /sys/ files or by controlling the Number of Threads created. [see my blog on NATIVE COMMAND QUEUEING]
    
    5. Active Range: It is defined as the range of LBAs that must be covered under the test. While writing, low Active Range simulates the behaviour of overprovisioning in SSD and short-stroking in HDDs. Thus, Low Active Range will give you a better performance.
    
    6. IOs Alignment and Un-Alignment: This parameter only applies to SSDs – this does not make any difference in the performance of HDDs. Aligning means keeping your LBAs, multiple of 8, 16 and 32 when doing Random Tests. This makes LBAs be 4K, 8K and 16K Page aligned respectively. And Alignment makes sure that while reading minimum Flash Pages are read and while writing, Alignment avoids what’s called Read-Modify-Write. Thus, when IOs are Aligned, performance improves. When IOs are un-aligned performance degrades. This utility also provides the Mix-Alignment feature in which all the LBAs will be covered.
    
    7. The entropy of data: Entropy is the amount of randomness in data. Our utility lets you generate data with 10 to 100 % Entropy.

The flow of the Code:
This utility is written in C Language and flow is very simple. It runs the selected test for a stipulated Test-Size; approximately. While running it provides Time Elapsed and Percentage of the Test Completion along with performance parameters, every 10 seconds (defined MACRO).

    1. Issue an Identify command and get “max_lba” (Maximum LBA a drive supports i.e. the drive size) variable initialized.
    2. Take all the Test Parameters like – Mix-Limited or Mix-unlimited, random or sequential, Block size, Read/write %, Alignment, Active Range, Start LBA in the case of sequential IOs and Entropy etc.
    3. Prepare the random data buffer as per the Entropy and Block-Size, if the test involves any Writes i.e when the Read-Percentage is not 100%.
    4. Lock the Mutex – I am using a mutex as threads share some global variables; The same mutex is also used to synchronise the threads for the sequential IOs. Here mutex has been locked so as to block (stop) the IOs generation after the creation of threads.
    5. Create the threads as per the Number of Threads, Mix-Limited and Mix-Unlimited and Random/Sequential IOs.
    6. Start taking the measurement after every 10 seconds (MACRO) until the test-size completes.
