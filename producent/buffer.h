//
// Created by bakoo on 21.01.2021.
//

#ifndef MODELMIESZANY_BUFFER_H
#define MODELMIESZANY_BUFFER_H

#include <stdbool.h>

struct buffer
{
    int first;
    int last;
    int size;
    int currentSize;
    int* buffer;

};

struct buffer* create(int size);
void delete(struct buffer* buffer);
int push(struct buffer* buffer, int element);
int pop(struct buffer* buffer);
int getCurrentSize(struct buffer* buffer);
bool isFull(struct buffer * buffer);


#endif //MODELMIESZANY_BUFFER_H
