# Remote models: Claude from your PC

A text model doesn't have to run on the PS5. A folder in `/data/prosperoai/models/` that
holds a `remote.json` file is a text model whose replies come from a chat server on
your home network. ProsperoAI streams the reply in as it arrives, and the conversation
is saved like any other.

The main use is **Claude through claude-bridge**. claude-bridge is a small program on
your PC that runs Claude Code under your own Claude login. Anything that speaks the
OpenAI chat format also works, such as Ollama or LM Studio.

## remote.json

```json
{
  "name": "Claude (PC)",
  "url": "http://192.168.1.20:8765",
  "token": "abcd-efgh-ijkl-mnop",
  "model": "claude"
}
```

| Field    | Meaning |
| -------- | ------- |
| `name`   | What Models shows. The folder name is used when this is missing. |
| `url`    | The server's address. `/v1/chat/completions` is added when it isn't there. |
| `token`  | Optional. Sent as `Authorization: Bearer <token>`. |
| `model`  | Optional. Passed to the server. For claude-bridge: `claude` (your default), `sonnet`, `opus` or `haiku`. |
| `system` | Optional system prompt. ProsperoAI's own prompt isn't sent, because it describes a model running on the console. |

claude-bridge writes this file for you, with your PC's address and token, in its
`ps5-model/claude-pc/` folder.

## Install

1. Start claude-bridge on the PC.
2. Copy its `ps5-model/claude-pc` folder to `/data/prosperoai/models/` on the PS5 (FTP),
   so you end up with `/data/prosperoai/models/claude-pc/remote.json`.
3. Start ProsperoAI, or refresh **Models**, and choose **Claude (PC)**.

Choosing a remote model frees the memory of the local model, the same as switching to
an image or voice model.

## Notes

- If your PC's address changes, edit `url` in `remote.json`. Giving the PC a fixed
  address in your router avoids this.
- The PS5's API on port 11434 serves remote models too. Set a key in `/app0/api_key.txt`
  so other devices on your network can't use your Claude through it.
- If the app offers an update, that update is the original ProsperoAI, which has no
  remote models. Skip it, or merge upstream into this fork and rebuild.
