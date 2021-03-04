//
// Created by bakoo on 21.01.2021.
//

#include "buffer.h"
#include <stdlib.h>
#include <stdio.h>


struct buffer* create(int size)
{
    struct buffer* buffer = (struct buffer*)calloc(1,sizeof(struct buffer));
    buffer->buffer = (int*)calloc(size,sizeof(int));
    buffer->size = size;
    buffer->first = 0;
    buffer->last = 0;
    buffer->currentSize = 0;
    return buffer;
}
void delete(struct buffer* buffer)
{
    free(buffer->buffer);
    free(buffer);
}
int push(struct buffer* buffer, int element)
{
    if (buffer->currentSize == buffer->size)
    {
        perror("Buffer full!\n");
        return -1;
    }
    buffer->buffer[buffer->last] = element;
    buffer->last = (buffer->last+1)%(buffer->size);
    buffer->currentSize++;
    return 0;
}
int pop(struct buffer* buffer)
{
    int dummy = buffer->buffer[buffer->first];
    buffer->buffer[buffer->first] = -1;
    buffer->first = (buffer->first+1)%buffer->size;
    buffer->currentSize--;

    return dummy;
}
int getCurrentSize(struct buffer* buffer)
{
    return buffer->currentSize;
}

bool isFull(struct buffer* buffer)
{
    return buffer->size == buffer->currentSize;
}