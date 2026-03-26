#include "agent.h"
#include "channel.h"
#include "commands.h"
#include "cmd-fs.h"
#include "protocol.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define FREEZE_POLL_TIMEOUT_MS 100

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
        /* Per QMP spec: return an error for malformed JSON.
         * The client handles stale data via guest-sync / 0xFF delimiter. */
        LOG_DEBUG("Parse error: %.40s%s", msg, strlen(msg) > 40 ? "..." : "");
        char *resp = protocol_build_error("GenericError", "JSON parse error", NULL);
        if (resp) {
            channel_send_response(ag->channel, resp);
            free(resp);
        }
        return;
    }

    const char *cmd_name = protocol_get_command(request);
    cJSON *args = protocol_get_arguments(request);
    const cJSON *id = protocol_get_id(request);

    if (!cmd_name) {
        /* Valid JSON but no "execute" field */
        char *resp = protocol_build_error("GenericError",
            "Missing 'execute' field", protocol_get_id(request));
        if (resp) {
            channel_send_response(ag->channel, resp);
            free(resp);
        }
        cJSON_Delete(request);
        return;
    }

    /* During freeze, only allow freeze-safe commands */
    if (!fsfreeze_command_allowed(cmd_name)) {
        char *resp = protocol_build_error("GenericError",
            "Command not allowed while filesystem is frozen", id);
        if (resp) {
            channel_send_response(ag->channel, resp);
            free(resp);
        }
        cJSON_Delete(request);
        return;
    }

    int use_delimiter = (strcmp(cmd_name, "guest-sync-delimited") == 0);

    /* Flush stale OUTPUT before writing the sync response.
     * Previous PVE sessions may have disconnected before reading all
     * our responses, leaving stale data in the serial output buffer.
     * This clears pending output only — does NOT touch the input
     * buffer where the ping command is waiting. */
    if (use_delimiter) {
        channel_flush_stale_output(ag->channel);
    }

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
        /* During freeze: shorten poll timeout and run continuous sync */
        if (fsfreeze_is_frozen()) {
            channel_set_poll_timeout(ag->channel, FREEZE_POLL_TIMEOUT_MS);
        } else {
            channel_set_poll_timeout(ag->channel, 1000);
        }

        char *msg = channel_read_message(ag->channel);
        if (!msg) {
            if (errno == EAGAIN) {
                /* Normal timeout — run continuous sync if frozen */
                if (fsfreeze_is_frozen()) {
                    fsfreeze_continuous_sync();
                }
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
