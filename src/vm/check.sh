#!/bin/bash
for ((i=100; i>=1; i--))
do
        echo $i;
	make clean;
	make check > "output/run$i.txt";
done

