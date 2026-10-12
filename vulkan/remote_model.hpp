// ProsperoAI - text models answered by another machine on the network.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A folder in the models directory that holds a remote.json is a text model whose replies
// come from an OpenAI-compatible chat server elsewhere on the network: claude-bridge on a
// PC (Claude through Claude Code), Ollama, LM Studio and the like. remote.json:
//
//   {
//     "name": "Claude (PC)",
//     "url": "http://192.168.1.20:8765",
//     "token": "abcd-efgh-ijkl-mnop",
//     "model": "claude"
//   }
//
// "url" is the server's address; /v1/chat/completions is added when it is not part of it.
// "token" (optional) is sent as a bearer key. "model" (optional) is passed to the server.
// "system" (optional) is sent as the system prompt; the app's own prompt is not sent,
// since it describes a model running on the console.
#pragma once

#include "gpt_runtime.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace prospero_remote
{
struct Endpoint
{
    std::string id; // the folder's name
    std::string name, url, token, model, system;
};

// The remote models in the folders of `root`, by name.
std::vector<Endpoint> discover(const char *root);

// One reply, streamed into `output` as it arrives; `progress` sees the text so far.
// 0 on success; otherwise nonzero with a message for the person in `output`.
int generate(const Endpoint &endpoint, const gpt_runtime_message_t *messages, unsigned count,
             char *output, std::size_t capacity, gpt_runtime_stats_t *stats,
             gpt_runtime_progress_fn progress);
} // namespace prospero_remote
