# multithreaded-rle-zip

A command line utility C program which utilizes N threads (decided by available system cores) to compress M input files in an efficient, parallelized fashion. 

The compression algorithm is based on lossless Run-length encoding (RLE), which condenses repeated characters into a five-byte chunk, comprised of a four-byte uint_32 to store the number of repeat characters, and one char to store the character itself.

![image](https://github.com/nibsuoogee/multithreaded-rle-zip/assets/37696410/d7101144-09c8-4ea5-a004-b7e6ae7d26bc)

![image](https://github.com/nibsuoogee/multithreaded-rle-zip/assets/37696410/9eeb6a03-d72e-413f-abb5-3e75449095dc)

