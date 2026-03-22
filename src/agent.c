#include "agent.h"
#include "channel.h"
#include "commands.h"
#include "protocol.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

struct agent {
    channel_t *channel;
    int        running;
    int        frozen;
    int        test_mode;
};

agent_t *agent_create(const char *device_path, int test_mode)
{
    agent_t *ag = calloc(1, sizeof(*ag));
    if (!ag) return NULL;

    if (test_mode) {
        ag->channel = channel_create_test();
    } else {
        ag->channel = channel_create(device_path);
    }

    if (!ag->channel) {
        free(ag);
        return NULL;
    }

    ag->test_mode = test_mode;
    return ag;
}

static void process_message(agent_t *ag, const char *msg)
{
    cJSON *request = protocol_parse_request(msg);
    if (!request) {
        /* Silently discard malformed messages — do NOT send error responses.
         * The sync-delimited protocol handles garbage via the \xff delimiter.
         * Sending error responses for parse failures adds stale data to the
         * serial pipe that corrupts future sync attempts. This matches
         * Linux qemu-ga behavior: discard garbage, don't respond. */
        LOG_DEBUG("Discarding malformed message: %.40s%s", msg, strlen(msg) > 40 ? "..." : "");
        return;
    }

    const char *cmd_name = protocol_get_command(request);
    cJSON *args = protocol_get_arguments(request);
    const cJSON *id = protocol_get_id(request);

    if (!cmd_name) {
        LOG_DEBUG("Discarding message with no execute field");
        cJSON_Delete(request);
        return;
    }

    int use_delimiter = (strcmp(cmd_name, "guest-sync-delimited") == 0);

    /* Note: do NOT flush buffers here. PVE sends sync + the actual
     * command (e.g. ping) in ONE write. Both commands are in our read
     * buffer. Flushing would destroy the second command. The \xff
     * delimiter in our sync response lets PVE skip any stale data. */

    char *resp = commands_dispatch(cmd_name, args, id);
    if (resp) {
        if (use_delimiter) {
            channel_send_delimited_response(ag->channel, resp);
        } else {
            channel_send_response(ag->channel, resp);
        }
        free(resp);
    }

    cJSON_Delete(request);
}

int agent_run(agent_t *ag)
{
    if (!ag) return -1;

    if (channel_open(ag->channel) != 0) {
        LOG_ERROR("Failed to open channel");
        return -1;
    }

    ag->running = 1;
    LOG_INFO("Agent started, listening for commands...");

    while (ag->running) {
        char *msg = channel_read_message(ag->channel);
        if (!msg) {
            if (errno == EAGAIN) {
                /* Normal timeout, continue polling */
                continue;
            }
            if (errno == 0 && ag->test_mode) {
                /* stdin closed or user typed quit */
                LOG_INFO("Test session ended");
                break;
            }
            if (errno == EIO) {
                LOG_ERROR("Device connection lost, attempting reconnect...");
                channel_close(ag->channel);
                sleep(5);
                if (channel_open(ag->channel) != 0) {
                    LOG_ERROR("Reconnect failed");
                    sleep(5);
                }
                continue;
            }
            /* Other error */
            LOG_ERROR("Read error (errno=%d), continuing...", errno);
            usleep(100000);
            continue;
        }

        process_message(ag, msg);
        free(msg);
    }

    channel_close(ag->channel);
    LOG_INFO("Agent stopped");
    return 0;
}

void agent_stop(agent_t *ag)
{
    if (ag) ag->running = 0;
}

void agent_destroy(agent_t *ag)
{
    if (!ag) return;
    channel_destroy(ag->channel);
    free(ag);
}

int agent_is_frozen(agent_t *ag)
{
    return ag ? ag->frozen : 0;
}

void agent_set_frozen(agent_t *ag, int frozen)
{
    if (ag) ag->frozen = frozen;
}
