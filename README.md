# multithreaded-rle-zip

A command line utility C program which utilizes N threads (decided by available system cores) to compress M input files in an efficient, parallelized fashion. 

The compression algorithm is based on lossless Run-length encoding (RLE), which condenses repeated characters into a five-byte chunk, comprised of a four-byte uint_32 to store the number of repeat characters, and one char to store the character itself.

# Pseudocode/abstract representation of the finished pzip/punzip programs

![image](https://github.com/nibsuoogee/multithreaded-rle-zip/assets/37696410/bdceb39c-0e36-4f5b-b49d-95e006630c16)

# Notes, hindsight and possible improvements

Positives
- The workload of compressing input files is initially divided fairly between threads
- Threads are able to complete the given work independently, by writing into their own output buffers, one for each file that the thread works on
- Once a given thread is done with compression, it moves on to concatenation, where it waits for other threads to complete buffers, and immediately concatenates output files once all dependent buffers are completed for the given output file

Possible improvements
- If some threads run faster than others, they may waste time idling while waiting to concatenate buffers after their own compression task.
  -  fix: distribute workload during the lifetime of threads dynamically, so that threads all work on compression until all output files are ready for concatenation.
  -  fix refactor workload/difficulty: relatively small I reckon. Currently, threads compress until they reach a byte quota. Finished threads could communicate with working threads to split this byte quota. Keeping track of multiple of the same thread's compression tasks for the same file is not supported, so this may prove to be a tougher challenge to refactor.
  -  In hindsight, a better approach could involve utilizing a pool of thread workers, that can be assigned compression tasks more fluently with changing performance of threads during execution.
- Larger files are poorly handled, as the threads will compress their sections of input files into buffers all at once, leading to a linear relationship between input file size and the program's memory usage.
  -  fix: Chunk processing could be utilized, so that threads do not compress the entire portion into memory at once, but handle a limited number of bytes at a time. Splitting into chunks could be done either before threads are created, so that more threads may be used, or the splitting could be done by threads, so that threads have more jobs.
  -  fix refactor workload/difficulty: again, relatively small, depending on where the splitting into chunks is done.

# Planning phase

![image](https://github.com/nibsuoogee/multithreaded-rle-zip/assets/37696410/d7101144-09c8-4ea5-a004-b7e6ae7d26bc)

![image](https://github.com/nibsuoogee/multithreaded-rle-zip/assets/37696410/9eeb6a03-d72e-413f-abb5-3e75449095dc)

