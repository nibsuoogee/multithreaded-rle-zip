#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#define handle_error(msg)   \
    do                      \
    {                       \
        perror(msg);        \
        exit(EXIT_FAILURE); \
    } while (0)

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int concat_signal = 0;
int num_files_completed = 0;
int num_files_glob;
int num_threads_glob;
int go = 0;
int threads_ready = 0;

typedef struct
{
    char *addr;
    off_t offset, pa_offset;
    size_t length;
    struct stat sb;
    char *file_name;
    char **comp_result_buffers; // will be of length num_threads, storing pointers to intermediate compression results from different threads
    size_t *buffer_lengths;
    int *finished_threads;
} mmapped_vars;

typedef struct
{
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
    thread_compress_struct *actual_args;
    int thread_id;
    char c;
    char prev_c;
    uint32_t count_c = 0;
    size_t buffer_length;
    size_t length;
    off_t st_size, current_buffer_max;
    off_t offset, pa_offset;
    double iter_memory_increase_mult = 1.5;

    pthread_mutex_lock(&mutex);
    threads_ready++;
    if (threads_ready < num_threads_glob)
    { // makes sure all threads are ready.. REMOVE THIS
        while (threads_ready < num_threads_glob)
        {
            pthread_cond_wait(&cond, &mutex);
        }
        pthread_mutex_unlock(&mutex);
    }
    else
    {
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mutex);
    }

    pthread_mutex_lock(&mutex);
    actual_args = args;
    thread_id = actual_args->thread_id;
    pthread_mutex_unlock(&mutex);

    // starting from mmapped_vars, index range_in_mvars_array_start
    // consume bytes (thread quota units) until sb.st_size or quota reaches zero
    // when current reaches sb.st_size on mmapped_vars index, move to next mmapped_vars index

    int current_mvar = actual_args->range_in_mvars_array_start;
    int offset_in_mvar = actual_args->offset_in_first_addr;

    // make copies of contentious vars
    pthread_mutex_lock(&mutex);
    memcpy(&length, &actual_args->mvars[current_mvar].length, sizeof(size_t));
    char *addr = actual_args->mvars[current_mvar].addr;
    memcpy(&st_size, &actual_args->mvars[current_mvar].sb.st_size, sizeof(off_t));
    memcpy(&offset, &actual_args->mvars[current_mvar].offset, sizeof(off_t));
    memcpy(&pa_offset, &actual_args->mvars[current_mvar].pa_offset, sizeof(off_t));
    pthread_mutex_unlock(&mutex);

    while (actual_args->bytes > 0)
    {

        if (actual_args->mvars[current_mvar].comp_result_buffers[thread_id] == NULL)
        {
            actual_args->mvars[current_mvar].comp_result_buffers[thread_id] = malloc(st_size * sizeof(char)); // for now, allocate the same amount as in original file mmap
            if (actual_args->mvars[current_mvar].comp_result_buffers[thread_id] == NULL)
            {
                handle_error("malloc");
            }
            buffer_length = 0;
            current_buffer_max = st_size;
        }

        if (st_size - offset_in_mvar <= actual_args->bytes)
        {
            actual_args->bytes -= (st_size - offset_in_mvar); // file mapping allocated to thread

            // read first character
            prev_c = *(char *)(addr + offset - pa_offset + offset_in_mvar);
            count_c = 1;
            offset_in_mvar += 1;
            while (offset_in_mvar < length)
            {
                c = *(char *)(addr + offset - pa_offset + offset_in_mvar);
                if (c == prev_c)
                { // if same, increment count_c
                    count_c++;
                }
                else
                { // if different, add count_c and c to output
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &count_c, sizeof(count_c));
                    buffer_length += sizeof(count_c);
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &prev_c, sizeof(prev_c));
                    buffer_length += sizeof(prev_c);
                    prev_c = c;
                    count_c = 1;
                    // increase buffer size if close to full..
                    if (buffer_length > current_buffer_max * 0.7)
                    {
                        char *temp = realloc(actual_args->mvars[current_mvar].comp_result_buffers[thread_id], (off_t)(current_buffer_max * iter_memory_increase_mult) * sizeof(char));
                        if (temp == NULL)
                        {
                            handle_error("realloc");
                        }
                        else
                        {
                            actual_args->mvars[current_mvar].comp_result_buffers[thread_id] = temp;
                            current_buffer_max = (off_t)(current_buffer_max * iter_memory_increase_mult);
                        }
                    }
                }
                offset_in_mvar++;
            }
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &count_c, sizeof(count_c));
            buffer_length += sizeof(count_c);
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &prev_c, sizeof(prev_c));
            buffer_length += sizeof(prev_c);
            actual_args->mvars[current_mvar].buffer_lengths[thread_id] = buffer_length;

            pthread_mutex_lock(&mutex);
            actual_args->mvars[current_mvar].finished_threads[thread_id]++; // thread's portion of file done/last portion
            concat_signal = 1;
            pthread_cond_broadcast(&cond); // Signal all waiting threads
            pthread_mutex_unlock(&mutex);

            current_mvar++;     // jump to next file mapping
            offset_in_mvar = 0; // previously partially completed file now fully completed
        }

        else
        {
            int limit_in_mvar = actual_args->bytes; // thread has no more byte quota =
            // continue to next thread, store partial compression offset completed by current thread

            actual_args->bytes = 0;

            prev_c = *(char *)(addr + offset - pa_offset + offset_in_mvar);
            count_c = 1;
            offset_in_mvar += 1;

            while (offset_in_mvar < limit_in_mvar)
            { // only compress until limit defined by insufficient quota for full compression
                c = *(char *)(addr + offset - pa_offset + offset_in_mvar);
                if (c == prev_c)
                { // if same, increment count_c
                    count_c++;
                }
                else
                { // if different, add count_c and c to output
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &count_c, sizeof(count_c));
                    buffer_length += sizeof(count_c);
                    memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &prev_c, sizeof(prev_c));
                    buffer_length += sizeof(prev_c);
                    prev_c = c;
                    count_c = 1;

                    // increase buffer size if close to full..
                    if (buffer_length > current_buffer_max * 0.7)
                    {
                        char *temp = realloc(actual_args->mvars[current_mvar].comp_result_buffers[thread_id], (off_t)(current_buffer_max * iter_memory_increase_mult) * sizeof(char));
                        if (temp == NULL)
                        {
                            handle_error("realloc");
                        }
                        else
                        {
                            actual_args->mvars[current_mvar].comp_result_buffers[thread_id] = temp;
                            current_buffer_max = (off_t)(current_buffer_max * iter_memory_increase_mult);
                        }
                    }
                }
                offset_in_mvar++;
            }

            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &count_c, sizeof(count_c));
            buffer_length += sizeof(count_c);
            memcpy(actual_args->mvars[current_mvar].comp_result_buffers[thread_id] + buffer_length, &prev_c, sizeof(prev_c));
            buffer_length += sizeof(prev_c);
            actual_args->mvars[current_mvar].buffer_lengths[thread_id] = buffer_length;

            pthread_mutex_lock(&mutex);
            actual_args->mvars[current_mvar].finished_threads[thread_id]++; // thread's portion of file done
            concat_signal = 1;
            pthread_cond_broadcast(&cond); // Signal all waiting threads
            pthread_mutex_unlock(&mutex);
        }
    }

    // thread has completed its own compression task, moves on to concatenation of ready intermediate buffers
    while (num_files_completed < num_files_glob)
    {
        pthread_mutex_lock(&mutex);

        while (concat_signal == 0 && num_files_completed < num_files_glob)
        {
            pthread_cond_wait(&cond, &mutex);
        }

        concat_signal = 0;
        if (num_files_completed < num_files_glob)
        {
            for (int i = 0; i < num_files_glob; i++)
            {
                int completion_sum = 0;
                for (int j = 0; j < num_threads_glob; j++)
                {
                    completion_sum += actual_args->mvars[i].finished_threads[j];
                }
                if (completion_sum == 0)
                {
                    actual_args->mvars[i].finished_threads[0]++; // invalidate concatenation of chosen file for other threads
                    num_files_completed++;
                    char outputFilename[256]; // Adjust the size as needed
                    snprintf(outputFilename, sizeof(outputFilename), "%s.z", actual_args->mvars[i].file_name);
                    FILE *outputFile = fopen(outputFilename, "wb");
                    if (outputFile == NULL)
                    {
                        handle_error("Error opening output file");
                    }
                    for (int thread = 0; thread < num_threads_glob; thread++)
                    {
                        if (actual_args->mvars[i].comp_result_buffers[thread] != NULL)
                        {
                            fwrite(actual_args->mvars[i].comp_result_buffers[thread], (int)actual_args->mvars[i].buffer_lengths[thread], 1, outputFile); // outputFile
                            free(actual_args->mvars[i].comp_result_buffers[thread]);
                            actual_args->mvars[i].comp_result_buffers[thread] = NULL;
                        }
                    }
                    fclose(outputFile);
                }
            }
        }
        pthread_mutex_unlock(&mutex);
    }

    free(actual_args);
    return NULL;
}

int main(int argc, char **argv, char *envp[])
{
    clock_t start, end;
    double cpu_time_used;
    start = clock();

    int fd;
    int num_files = argc - 1;
    num_files_glob = num_files;
    mmapped_vars mvars[num_files]; // store map and info for each input file
    int num_threads = 4;           // get_nprocs();
    num_threads_glob = num_threads;
    pthread_t fids[num_threads];

    int total_bytes = 0;
    int bytes_per_thread, remainingBytes;

    if (argc < 2)
    {
        printf("usage: pzip <input> > <output>\n");
        return (1);
    }

    // for loop mmap() over files
    for (int file = 1; file < argc; file++)
    {

        fd = open(argv[file], O_RDONLY);
        if (fd == -1)
            handle_error("open");

        if (fstat(fd, &mvars[file - 1].sb) == -1) /* To obtain file size */
            handle_error("fstat");

        mvars[file - 1].offset = 0; // atoi(argv[2]);
        mvars[file - 1].pa_offset = mvars[file - 1].offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
        /* offset for mmap() must be page aligned */

        if (mvars[file - 1].offset >= mvars[file - 1].sb.st_size)
        {
            handle_error("offset is past end of file\n");
        }
        total_bytes += mvars[file - 1].sb.st_size;

        mvars[file - 1].length = mvars[file - 1].sb.st_size - mvars[file - 1].offset;

        mvars[file - 1].addr = mmap(NULL, mvars[file - 1].length + mvars[file - 1].offset - mvars[file - 1].pa_offset, PROT_READ,
                                    MAP_PRIVATE, fd, mvars[file - 1].pa_offset);
        if (mvars[file - 1].addr == MAP_FAILED)
            handle_error("mmap");

        mvars[file - 1].file_name = argv[file];

        mvars[file - 1].comp_result_buffers = malloc(num_threads * sizeof(char *));
        if (mvars[file - 1].comp_result_buffers == NULL)
        {
            handle_error("malloc");
        }
        close(fd);
        printf("mvars[%d - 1].comp_result_buffers %p\n", file, (void *)mvars[file - 1].comp_result_buffers);

        for (int thread = 0; thread < num_threads; thread++)
        {
            mvars[file - 1].comp_result_buffers[thread] = NULL;
        }

        mvars[file - 1].buffer_lengths = malloc(sizeof(size_t) * num_threads);
        if (mvars[file - 1].buffer_lengths == NULL)
        {
            handle_error("malloc");
        }
        for (int i = 0; i < num_threads; ++i)
        {
            mvars[file - 1].buffer_lengths[i] = 0;
        }
        printf("mvars[%d - 1].buffer_lengths %p\n", file, (void *)mvars[file - 1].buffer_lengths);
        mvars[file - 1].finished_threads = (int *)malloc(sizeof(int) * (num_threads));
        if (mvars[file - 1].finished_threads == NULL)
        {
            handle_error("malloc");
        }
        for (int i = 0; i < num_threads; ++i)
        {
            mvars[file - 1].finished_threads[i] = 0;
        }
        printf("file %d bytes %ld\n", file - 1, mvars[file - 1].sb.st_size);
    }

    bytes_per_thread = total_bytes / num_threads;
    remainingBytes = total_bytes % num_threads;

    // create threads, give each a range for mvars, a pointer to mvars
    // their assigned byte amount, and offset within their first file
    int current_mvar = 0;
    int offset_into_next_mvar = 0;
    for (int i = 0; i < num_threads; i++)
    {
        thread_compress_struct *args = malloc(sizeof *args);
        if (args == NULL)
        {
            handle_error("malloc");
        }
        args->thread_id = i;
        args->mvars = mvars;
        args->bytes = bytes_per_thread + (i < remainingBytes ? 1 : 0);
        args->range_in_mvars_array_start = current_mvar;
        args->offset_in_first_addr = offset_into_next_mvar;

        int bytes_left_for_thread = args->bytes;

        while (bytes_left_for_thread > 0)
        {
            args->range_in_mvars_array_end = current_mvar; // shift the last file thread is responsible for
            if (mvars[current_mvar].sb.st_size - offset_into_next_mvar <= bytes_left_for_thread)
            {
                bytes_left_for_thread -= (mvars[current_mvar].sb.st_size - offset_into_next_mvar); // file mapping allocated to thread

                mvars[current_mvar].finished_threads[i]--; // decrement number of threads that must work on this input file

                current_mvar++;            // jump to next file mapping
                offset_into_next_mvar = 0; // previously partially completed file now fully completed
            }
            else
            {
                offset_into_next_mvar += bytes_left_for_thread; // thread has no more byte quota =
                // continue to next thread, store partial compression offset completed by current thread
                bytes_left_for_thread = 0;

                mvars[current_mvar].finished_threads[i]--; // decrement number of threads that must work on this input file
            }
        }
        /*
        printf("Plan:\n");
        for (int file = 0; file < num_files_glob; file++)
        {
            printf("completion file %d, thread %d: %d \n", file+1, i, mvars[file].finished_threads[i]);
        }
        printf("Plan ended\n");
        */
        if (pthread_create(&fids[i], NULL, compress, args) != 0)
        {
            handle_error("pthread_create");
        }
    }

    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_join(fids[i], NULL) != 0)
        {
            handle_error("pthread_join");
        }
    }

    printf("Results:\n");
    for (int thread = 0; thread < num_threads_glob; thread++)
    {
        for (int i = 0; i < num_files_glob; i++)
        {
            printf("completion file %d, thread %d: %d \n", i + 1, thread, mvars[i].finished_threads[thread]);
        }
        printf("\n");
    }
    printf("Results ended\n");

    for (int file = 1; file < argc; file++)
    {
        munmap(mvars[file - 1].addr, mvars[file - 1].length + mvars[file - 1].offset - mvars[file - 1].pa_offset);
    }
    for (int file = 1; file < argc; file++)
    {
        free(mvars[file - 1].comp_result_buffers);
        free(mvars[file - 1].buffer_lengths);
        free(mvars[file - 1].finished_threads);
    }

    end = clock();
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("main took %f seconds to execute \n", cpu_time_used);
    exit(EXIT_SUCCESS);
}