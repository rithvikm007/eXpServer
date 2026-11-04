#include "xps.h"
#include <stdlib.h>
#include <string.h>

void test_handler(void *ptr) {
    // Dummy handler for test endpoints
}

void test_close_handler(void *ptr) {
    // Dummy close handler for test endpoints
}

int main() {
    // Create core
    xps_core_t *core = xps_core_create(NULL);
    assert(core != NULL);

    // Create GZIP instance
    xps_gzip_t *gzip = xps_gzip_create(Z_DEFAULT_COMPRESSION);
    assert(gzip != NULL);

    // Create dummy input source
    xps_pipe_source_t *input_source = xps_pipe_source_create(
        core, test_handler, test_close_handler
    );
    assert(input_source != NULL);

    // Create dummy output sink
    xps_pipe_sink_t *output_sink = xps_pipe_sink_create(
        core, test_handler, test_close_handler
    );
    assert(output_sink != NULL);

    // Connect input source to GZIP sink
    xps_pipe_t *input_pipe = xps_pipe_create(
        core,
        DEFAULT_PIPE_BUFF_THRESH,
        input_source,
        gzip->sink
    );
    assert(input_pipe != NULL);

    // Connect GZIP source to output sink
    xps_pipe_t *output_pipe = xps_pipe_create(
        core,
        DEFAULT_PIPE_BUFF_THRESH,
        gzip->source,
        output_sink
    );
    assert(output_pipe != NULL);

    // Generate 256 KB of deterministic input data
    size_t input_len = 256 * 1024;

    xps_buffer_t *input_buff = xps_buffer_create(input_len, input_len, NULL);
    assert(input_buff != NULL);

    unsigned int state = 123456789;

    for (size_t i = 0; i < input_len; i++) {
        input_buff->data[i] = 'A';
    }

    // Save original input
    FILE *input_file = fopen("gzip_input.bin", "wb");
    assert(input_file != NULL);
    assert(fwrite(input_buff->data, 1, input_len, input_file) == input_len);
    fclose(input_file);

    // Write input into the input pipe
    assert(xps_pipe_source_write(input_source, input_buff) == OK);
    xps_buffer_destroy(input_buff);

    // Process input through GZIP sink
    gzip->sink->handler_cb(gzip->sink);

    // Forward the first compressed output
    assert(gzip->transfer_buff != NULL);
    assert(gzip->source->ready);
    gzip->source->handler_cb(gzip->source);

    // Simulate input source closing
    assert(xps_pipe_detach_source(input_pipe) == OK);
    xps_pipe_source_destroy(input_source);

    // Simulate the event loop marking the sink inactive
    gzip->sink->active = false;

    // Finish GZIP compression (Z_FINISH)
    gzip->sink->close_cb(gzip->sink);

    // Forward final compressed output
    assert(gzip->transfer_buff != NULL);
    assert(gzip->source->ready);
    gzip->source->handler_cb(gzip->source);

    // Read compressed data from output pipe
    size_t compressed_len = output_pipe->buff_list->len;
    assert(compressed_len > 0);

    xps_buffer_t *compressed_buff = xps_pipe_sink_read(
        output_sink,
        compressed_len
    );
    assert(compressed_buff != NULL);

    // Save compressed output
    FILE *output_file = fopen("gzip_output.gz", "wb");
    assert(output_file != NULL);
    assert(fwrite(compressed_buff->data, 1, compressed_buff->len, output_file)
           == compressed_buff->len);
    fclose(output_file);

    printf("Original size:   %zu bytes\n", input_len);
    printf("Compressed size: %zu bytes\n", compressed_buff->len);

    xps_buffer_destroy(compressed_buff);

    // Destroy remaining core resources
    // GZIP instance has already destroyed itself.
    xps_core_destroy(core);

    printf("GZIP test completed.\n");

    return 0;
}