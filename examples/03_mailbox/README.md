# Example 03: Mailbox IPC

Two guests cooperating via the mailbox subsystem — the foundation
of the "VM OS" model where independent programs communicate by
sending typed messages to each other's mailboxes.

## What this demonstrates

- **Multi-VM scheduling**: two VMs registered with one
  `VmSystem`; the scheduler round-robins between them.
- **`SYS_WHITELIST_ADD`**: each mailbox controls who's allowed to
  send to it. The consumer adds the producer to its allow-list
  before any messages can flow.
- **`SYS_SEND` and `SYS_RECV`**: the actual byte-passing primitive.
  Messages are fixed-size (32 bytes by default) — a slot, not a
  stream. Slot count and slot size are configurable per mailbox.
- **Cooperative startup**: if the producer runs before the
  consumer has whitelisted it, `SYS_SEND` returns `-EPERM`. The
  producer handles this by yielding and retrying, so the order in
  which the scheduler picks them up doesn't matter.
- **Synchronous delivery**: when the consumer is blocked on
  `SYS_RECV` and the producer sends, the kernel copies the
  payload directly into the consumer's destination buffer and
  unblocks it. The message never enters the queue. Faster than
  enqueue-then-dequeue, and the consumer's `SYS_RECV` returns
  with the sender's VM id in `a0`.

## Build and run

```
./build.sh run
```

Expected output:

```
03_mailbox: compiling host...
03_mailbox: compiling producer (RV32IMC)...
03_mailbox: compiling consumer (RV32IMC)...
03_mailbox: built. To run:
    .../examples/03_mailbox/build/host

03_mailbox: ===== running =====
host: loaded producer (vm 0) and consumer (vm 1)
host: running until both halt...

consumer got: 0 from VM 0
consumer got: 1 from VM 0
consumer got: 2 from VM 0
...
consumer got: 9 from VM 0
consumer: done

host: all guests halted
```

The interleaving of stderr (host messages) and stdout (consumer
output) depends on your terminal's buffering policy; in a TTY
both are line-buffered and you'll see the consumer lines as they
happen.

## Files

- `host.c` — loads both guests, runs the scheduler until both halt
- `producer.c` — sends 10 counter values to VM 1
- `consumer.c` — whitelists VM 0, receives 10 messages, prints
- `build.sh` — compiles all three, optionally runs

## What's next

- **04_keydump** — raw-mode terminal input

This example is the foundation of the "shell VM" pattern: a
control-plane guest (consumer) and a worker guest (producer) that
talk over mailboxes. Real systems would extend this with:

- More guests (the scheduler supports up to 64)
- A message-type tag in the payload (we only send a counter here)
- The consumer routing messages by VM id or type
- Larger slot sizes for richer payloads
