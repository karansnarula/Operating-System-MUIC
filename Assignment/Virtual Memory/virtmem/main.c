/*
Main program for the virtual memory project.
Make all of your modifications to this file.
You may add or rearrange any code or data as you need.
The header files page_table.h and disk.h explain
how to use the page table and disk interfaces.
*/

#include "page_table.h"
#include "disk.h"
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


int count_diskread = 0;
int count_diskwrite = 0;
int count_pagefault = 0;
int counter = 0;
int *array;
struct disk *disk;
int replacement_algorithm;


int search(int l, int r, int key)
{
    int m;
    for(m=l;m<=r;m++){
        if(array[m] == key){
            return m;
        }
    }
    return -1;
}


void page_fault_handler( struct page_table *pt, int page )
{
	int pages_number = page_table_get_npages(pt);
    int frames_number = page_table_get_nframes(pt);
    char *physmem = page_table_get_physmem(pt);

    if(frames_number >= pages_number){
    	printf("page fault on page #%d\n",page);
    	page_table_set_entry(pt,page,page,PROT_READ|PROT_WRITE);
    	count_pagefault++;
    	count_diskread=0;
    	count_diskwrite=0;
    }
    // Random Replacement
    else if(replacement_algorithm == 1){
            count_pagefault++;
            int k = search(0,frames_number-1,page);
            int temp = lrand48()%frames_number;
            if(k > -1){
            	page_table_set_entry(pt,page,k,PROT_READ|PROT_WRITE);
            	count_pagefault--;
            }
            else if(counter < frames_number){
            	while(array[temp]!= -1){
            		temp = lrand48()%frames_number;
            		count_pagefault++;
            	}
            	page_table_set_entry(pt,page,temp,PROT_READ);
            	disk_read(disk,page,&physmem[temp*PAGE_SIZE]);
            	count_diskread++;
            	array[temp]=page;
            	counter++;
            }
            else{
            	disk_write(disk,array[temp],&physmem[temp*PAGE_SIZE]);
            	disk_read(disk,page,&physmem[temp*PAGE_SIZE]);
            	count_diskread++;
            	count_diskwrite++;
            	page_table_set_entry(pt,page,temp,PROT_READ);
            	array[temp]=page;
            }
            page_table_print(pt);
    }
    // FIFO Replacement
    else if(replacement_algorithm == 2){
            count_pagefault++;
            int k = search(0,frames_number-1,page);
            if(k > -1){
                page_table_set_entry(pt,page,k,PROT_READ|PROT_WRITE);
                counter--;
                count_pagefault--;
            }
            else if(array[counter]==-1){
                page_table_set_entry(pt,page,counter,PROT_READ);
                disk_read(disk,page,&physmem[counter*PAGE_SIZE]);
                count_diskread++;
            }
            else{
                disk_write(disk,array[counter],&physmem[counter*PAGE_SIZE]);
                disk_read(disk,page,&physmem[counter*PAGE_SIZE]);
                count_diskread++;
                count_diskwrite++;
                page_table_set_entry(pt,page,counter,PROT_READ);
                }
                array[counter]=page;
                counter=(counter+1)%frames_number;
                page_table_print(pt);
    }
    // LRU Replacement
    else{
        count_pagefault++;
        int k = search(0,frames_number-1,page);
        if(k > -1){
            page_table_set_entry(pt,page,k,PROT_READ|PROT_WRITE);
            count_pagefault--;
        }
        else if(array[counter]==-1){
            page_table_set_entry(pt,page,counter,PROT_READ);
            disk_read(disk,page,&physmem[counter*PAGE_SIZE]);
            count_diskread++;
            count_diskwrite++;
        }
        else{
            disk_write(disk,array[counter],&physmem[counter*PAGE_SIZE]);
            disk_read(disk,page,&physmem[counter*PAGE_SIZE]);
            count_diskread++;
            page_table_set_entry(pt,page,counter,PROT_READ);
            }
            array[counter]=page;
            counter=(counter+1)%frames_number;
            counter--;
            page_table_print(pt);
    }
	//exit(1);
}

int main( int argc, char *argv[] )
{
	if(argc!=5) {
		printf("use: virtmem <npages> <nframes> <rand|fifo|lru> <sort|scan|focus>\n");
		return 1;
	}

	int npages = atoi(argv[1]);
	int nframes = atoi(argv[2]);
    if(!strcmp(argv[3],"rand")){
        replacement_algorithm = 1;
    }
    if(!strcmp(argv[3],"fifo")){
        replacement_algorithm = 2;
    }
    if(!strcmp(argv[3],"lru")){
        replacement_algorithm = 3;
    }
	const char *program = argv[4];

	array=(int *)malloc(nframes*sizeof(int));
	int i;
	for(i=0;i<nframes;i++)
    array[i]=-1;

	disk = disk_open("myvirtualdisk",npages);

	if(!disk) {
		fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
		return 1;
	}


	struct page_table *pt = page_table_create( npages, nframes, page_fault_handler );
	if(!pt) {
		fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
		return 1;
	}

	char *virtmem = page_table_get_virtmem(pt);


	if(!strcmp(program,"sort")) {
		sort_program(virtmem,npages*PAGE_SIZE);

	} else if(!strcmp(program,"scan")) {
		scan_program(virtmem,npages*PAGE_SIZE);

	} else if(!strcmp(program,"focus")) {
		focus_program(virtmem,npages*PAGE_SIZE);

	} else {
		fprintf(stderr,"unknown program: %s\n",argv[4]);

	}

	page_table_delete(pt);
	disk_close(disk);

	printf("\nSUMMARY:\n");
	printf("No. of page faults:%d\n",count_pagefault);
	printf("No. of disk reads:%d\n",count_diskread);
	printf("No. of disk writes:%d\n",count_diskwrite);

	return 0;
}
