#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#define handle_error(msg) \
    do { perror(msg); exit(EXIT_FAILURE); } while (0)

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int num_files_ready_for_concat = 0;
int num_files_completed = 0;
int *files_readiness_to_concat;
int num_files_glob;
int num_threads_glob;

typedef struct {
    char *addr;
    off_t offset, pa_offset, current;
    size_t length;
    ssize_t s;
    struct stat sb;
    char *file_name;
    char **comp_result_buffers; // will be of length num_threads, storing pointers to intermediate compression results from different threads 
    size_t *buffer_offsets;
} mmapped_vars;

typedef struct {
    // create threads, give each a range for mvars, a pointer to mvars
    // their assigned byte amount, and offset within their first file
    mmapped_vars *mvars;
    int range_in_mvars_array_start, range_in_mvars_array_end;
    int bytes;
    int offset_in_first_addr;
    int thread_id;
} thread_compress_struct;

void *compress(void *args)
{
    char c;
    char prev_c;
    uint32_t count_c = 0;
    thread_compress_struct *actual_args = args;
    int thread_id = actual_args->thread_id;
    size_t *buffer_offset;
    mmapped_vars *current_mvar_vars;
    
    // starting from mmapped_vars, index range_in_mvars_array_start
    // consume bytes (thread quota units) until sb.st_size or quota reaches zero
    // when current reaches sb.st_size on mmapped_vars index, move to next mmapped_vars index 
    
    int current_mvar = actual_args->range_in_mvars_array_start;
    int offset_in_mvar = actual_args->offset_in_first_addr;

    //current_mvar_vars = &(actual_args->mvars[current_mvar]);

    // allocate space in the intermediates buffer for given file
    actual_args->mvars[current_mvar].comp_result_buffers;
    actual_args->mvars[current_mvar].buffer_offsets[thread_id];

    while (actual_args->bytes > 0){
        if (actual_args->mvars[current_mvar].comp_result_buffers[thread_id] == NULL) {
            actual_args->mvars[current_mvar].comp_result_buffers[thread_id] = malloc(actual_args->mvars[current_mvar].length); // for now, allocate the same amount as in original file mmap
            if (actual_args->mvars[current_mvar].comp_result_buffers[thread_id] == NULL) {
                // Handle allocation failure
                perror("malloc");
                exit(EXIT_FAILURE);
            }
            buffer_offset = &(actual_args->mvars[current_mvar].buffer_offsets[thread_id]);
            *buffer_offset = 0;
        }
        if (actual_args->mvars[current_mvar].sb.st_size - offset_in_mvar <= actual_args->bytes) {
            actual_args->bytes -= (actual_args->mvars[current_mvar].sb.st_size - offset_in_mvar); // file mapping allocated to thread
            // read first character
            prev_c = *(char *)(actual_args->mvars[current_mvar].addr + actual_args->mvars[current_mvar].offset - actual_args->mvars[current_mvar].pa_offset + offset_in_mvar);
            count_c = 1;
            offset_in_mvar += 1;
            
            while(offset_in_mvar < actual_args->mvars[current_mvar].length) { 
                c = *(char *)(actual_args->mvars[current_mvar].addr + actual_args->mvars[current_mvar].offset - actual_args->mvars[current_mvar].pa_offset + offset_in_mvar);
                if(c == prev_c){ // if same, increment count_c
                    count_c++;
                } else { // if different, add count_c and c to output
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &count_c, sizeof(count_c));
                    *buffer_offset += sizeof(count_c);
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &prev_c, sizeof(prev_c));
                    *buffer_offset += sizeof(prev_c);
                    printf("Tid %d: prev_c: %c\n", thread_id, prev_c);
                    prev_c = c;
                    count_c = 1;
                }
                offset_in_mvar++;
            }
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &count_c, sizeof(count_c));
            *buffer_offset += sizeof(count_c);
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &prev_c, sizeof(prev_c));
            *buffer_offset += sizeof(prev_c);
            printf("Tid %d: prev_c: %c\n", thread_id, prev_c);
            
            pthread_mutex_lock(&mutex);
            printf("Thread %d: file %d ready for concat\n", thread_id, current_mvar);
            num_files_ready_for_concat++;
            files_readiness_to_concat[current_mvar] = 1;
            pthread_cond_broadcast(&cond); // Signal all waiting threads
            pthread_mutex_unlock(&mutex);
            
            current_mvar++; // jump to next file mapping
            offset_in_mvar = 0; // previously partially completed file now fully completed
            
        } else {
            int limit_in_mvar = actual_args->bytes; // thread has no more byte quota = 
            // continue to next thread, store partial compression offset completed by current thread
            actual_args->bytes = 0;

            prev_c = *(char *)(actual_args->mvars[current_mvar].addr + actual_args->mvars[current_mvar].offset - actual_args->mvars[current_mvar].pa_offset + offset_in_mvar);
            count_c = 1;
            offset_in_mvar += 1;
            while(offset_in_mvar < limit_in_mvar) { // only compress until limit defined by insufficient quota for full compression
                c = *(char *)(actual_args->mvars[current_mvar].addr + actual_args->mvars[current_mvar].offset - actual_args->mvars[current_mvar].pa_offset + offset_in_mvar);
                if(c == prev_c){ // if same, increment count_c
                    count_c++;
                } else { // if different, add count_c and c to output
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &count_c, sizeof(count_c));
                    *buffer_offset += sizeof(count_c);
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &prev_c, sizeof(prev_c));
                    *buffer_offset += sizeof(prev_c);
                    printf("Tid %d: prev_c: %c\n", thread_id, prev_c);

                    prev_c = c;
                    count_c = 1;
                }
                offset_in_mvar++;
            }
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &count_c, sizeof(count_c));
            *buffer_offset += sizeof(count_c);
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + *buffer_offset, &prev_c, sizeof(prev_c));
            *buffer_offset += sizeof(prev_c);
        }
        
    }

    printf("now thread %d concatting\n", thread_id);

    while (num_files_completed < num_files_glob) {
        pthread_mutex_lock(&mutex);

        while (num_files_ready_for_concat == 0 && (num_files_completed < num_files_glob)) {
            pthread_cond_wait(&cond, &mutex);
        }
        if (num_files_completed < num_files_glob) {
            for (int i = 0; i < num_files_glob; i++){
                if (files_readiness_to_concat[i] == 1) {
                    files_readiness_to_concat[i] = 0;
                    
                    char outputFilename[256];  // Adjust the size as needed
                    snprintf(outputFilename, sizeof(outputFilename), "%s.z", actual_args->mvars[i].file_name);

                    FILE *outputFile = fopen(outputFilename, "wb");
                    if (outputFile == NULL) {
                        perror("Error opening output file");
                        exit(EXIT_FAILURE);
                    }

                    for (int thread = 0; thread < num_threads_glob; thread++){
                        if (actual_args->mvars[i].comp_result_buffers[thread] != NULL) {
                            //printf("file %d, thread %d:", i, thread);
                            fwrite(actual_args->mvars[i].comp_result_buffers[thread], (int)actual_args->mvars[i].buffer_offsets[thread], 1, outputFile); // outputFile
                            //printf("\n");
                        }
                    }

                    fclose(outputFile);

                    num_files_completed++;
                    num_files_ready_for_concat--;
                    printf("Thread %d performed action on file %d\n", actual_args->thread_id, i);
                    printf("num_files_completed: %d\n",num_files_completed);
                    printf("num_files_ready_for_concat: %d\n",num_files_ready_for_concat);
                }
            }
        }
        
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}
/*
void *concat(void *args) {
    thread_compress_struct *actual_args = args;
    printf("now thread concatting\n");
    
    while (num_files_completed < num_files_glob) {
        pthread_mutex_lock(&mutex);

        while (a_file_is_ready_for_concat != 1) {
            pthread_cond_wait(&cond, &mutex);
        }

        for (int i = 0; i < num_files_glob; i++){
            if (files_readiness_to_concat[i] == 1) {
                
                char outputFilename[256];  // Adjust the size as needed
                snprintf(outputFilename, sizeof(outputFilename), "%s.z", actual_args->mvars[i].file_name);

                FILE *outputFile = fopen(outputFilename, "wb");
                if (outputFile == NULL) {
                    perror("Error opening output file");
                    exit(EXIT_FAILURE);
                }

                for (int thread = 0; thread < num_threads_glob; thread++){
                    if (actual_args->mvars[i].comp_result_buffers[thread] != NULL) {
                        printf("file %d, thread %d:", i, thread);
                        fwrite(actual_args->mvars[i].comp_result_buffers[thread], (int)actual_args->mvars[i].buffer_offsets[thread], 1, outputFile); // outputFile
                        printf("\n");
                    }
                }

                fclose(outputFile);

                printf("Thread %d performed action on file index %d\n", actual_args->thread_id, i);
            }
        }

        pthread_mutex_unlock(&mutex);
    }
    
    return NULL;
}
*/
int main(int argc, char** argv, char *envp[])
{
    clock_t start, end;
    double cpu_time_used;
    start = clock();

    FILE *zfptr;
    int fd;
    int num_files = argc-1;
    num_files_glob = num_files; 
    mmapped_vars mvars[num_files]; // store map and info for each input file
    int num_threads = get_nprocs();
    num_threads_glob = num_threads; 
    //printf("num_threads: %d\n",num_threads);
    pthread_t fids[num_threads];
    pthread_t concat_fids[num_threads];

    int total_bytes = 0;
    int bytes_per_thread, remainingBytes;

    if (argc < 2) {
        printf("usage: pzip <input> > <output>\n");
        return(1);
    }

    files_readiness_to_concat = (int*)malloc(sizeof(int) * (num_files));
    if (files_readiness_to_concat == NULL) {
        perror("malloc()");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < num_files; ++i) {
        files_readiness_to_concat[i] = 0;
    }
    
    // for loop mmap() over files
    for (int file = 1; file < argc; file++){

        fd = open(argv[file], O_RDONLY);
        if (fd == -1)
            handle_error("open"); 

        if (fstat(fd, &mvars[file-1].sb) == -1) /* To obtain file size */
        handle_error("fstat");

        mvars[file-1].offset = 0; //atoi(argv[2]);
            mvars[file-1].pa_offset = mvars[file-1].offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
                /* offset for mmap() must be page aligned */

        if (mvars[file-1].offset >= mvars[file-1].sb.st_size) {
            fprintf(stderr, "offset is past end of file\n");
            exit(EXIT_FAILURE);
        }
        total_bytes += mvars[file-1].sb.st_size;

        mvars[file-1].length = mvars[file-1].sb.st_size - mvars[file-1].offset;

        mvars[file-1].addr = mmap(NULL, mvars[file-1].length + mvars[file-1].offset - mvars[file-1].pa_offset, PROT_READ,
            MAP_PRIVATE, fd, mvars[file-1].pa_offset);
        if (mvars[file-1].addr == MAP_FAILED)
            handle_error("mmap");
        printf("mvars[file-1].sb.st_size %ld\n", mvars[file-1].sb.st_size);
        mvars[file-1].file_name = argv[file];

        mvars[file-1].comp_result_buffers = malloc(num_threads * sizeof(char*));
        if (mvars[file-1].comp_result_buffers == NULL) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
        close(fd);

        for (int thread = 0; thread < num_threads; thread++){
            mvars[file-1].comp_result_buffers[thread] = NULL;
        }

        mvars[file-1].buffer_offsets = malloc(sizeof(size_t));
        if (mvars[file-1].buffer_offsets == NULL) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    //printf("total_bytes: %d\n", total_bytes);

    bytes_per_thread = total_bytes/num_threads;
    //printf("bytes_per_thread: %d\n", bytes_per_thread);
    
    remainingBytes = total_bytes % num_threads;
    //printf("remainingBytes: %d\n", remainingBytes);
    
    // create threads, give each a range for mvars, a pointer to mvars
    // their assigned byte amount, and offset within their first file
    int current_mvar = 0;
    int offset_into_next_mvar = 0;
    int bytes_for_this_thread;
    for (int i = 0; i < num_threads; i++){
        thread_compress_struct *args = malloc(sizeof *args);
        if (args == NULL) {
            perror("args");
            exit(EXIT_FAILURE);
        }
        args->thread_id = i;
        args->mvars = mvars;
        args->bytes = bytes_per_thread + (i < remainingBytes ? 1 : 0);
        args->range_in_mvars_array_start = current_mvar;
        args->offset_in_first_addr = offset_into_next_mvar;
        
        
        int bytes_left_for_thread = args->bytes;
        
        while (bytes_left_for_thread > 0){
            //printf("T%d: start bytes_left_for_thread: %d\n", i+1, bytes_left_for_thread);
            args->range_in_mvars_array_end = current_mvar; // shift the last file thread is responsible for
            if (mvars[current_mvar].sb.st_size - offset_into_next_mvar <= bytes_left_for_thread) {
                bytes_left_for_thread -= (mvars[current_mvar].sb.st_size - offset_into_next_mvar); // file mapping allocated to thread
                current_mvar++; // jump to next file mapping
                offset_into_next_mvar = 0; // previously partially completed file now fully completed
            } else {
                offset_into_next_mvar += bytes_left_for_thread; // thread has no more byte quota = 
                // continue to next thread, store partial compression offset completed by current thread
                bytes_left_for_thread = 0;
            }
            //printf("T%d: end bytes_left_for_thread: %d\n", i+1, bytes_left_for_thread);
        }
        
        if(pthread_create(&fids[i], NULL, compress, args) != 0) {
            free(args);
            perror("pthread_create");
            exit(1);
        }
        
    }
    
    for (int i = 0; i < num_threads; i++){
        if (pthread_join(fids[i], NULL) != 0) {
            perror("pthread_join");
            exit(1);
        }
        /*
        thread_compress_struct *args_conc = malloc(sizeof *args_conc);
        if (args_conc == NULL) {
            perror("args_conc");
            exit(EXIT_FAILURE);
        }
        args_conc->thread_id = i;
        args_conc->mvars = mvars;
        if(pthread_create(&concat_fids[i], NULL, concat, args_conc) != 0) {
            perror("pthread_create");
            exit(1);
        }
        if (pthread_join(concat_fids[i], NULL) != 0) {
            perror("pthread_join");
            exit(1);
        }
        */
    }

    printf("\n");
    /*
    for (int file = 1; file < argc; file++){
        char outputFilename[256];  // Adjust the size as needed
        snprintf(outputFilename, sizeof(outputFilename), "%s.z", mvars[file-1].file_name);

        FILE *outputFile = fopen(outputFilename, "wb");
        if (outputFile == NULL) {
            perror("Error opening output file");
            exit(EXIT_FAILURE);
        }

        for (int thread = 0; thread < num_threads; thread++){
            if (mvars[file-1].comp_result_buffers[thread] != NULL) {
                printf("file %d, thread %d:", file, thread);
                fwrite(mvars[file-1].comp_result_buffers[thread], (int)mvars[file-1].buffer_offsets[thread], 1, outputFile); // outputFile
                printf("\n");
            }
        }

        fclose(outputFile);
    }
    */

    for (int file = 1; file < argc; file++){
        munmap(mvars[file-1].addr, mvars[file-1].length + mvars[file-1].offset - mvars[file-1].pa_offset);
    }
    
    end = clock();
    cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
    printf("main took %f seconds to execute \n", cpu_time_used); 
    exit(EXIT_SUCCESS);
}