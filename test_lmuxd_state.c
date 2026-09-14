#define LMUXD_NO_MAIN
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "lmuxd.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>

static int fail(const char *msg)
{
    fprintf(stderr, "lmuxd state test failed: %s\n", msg);
    return 1;
}

static void init_app(struct app *a)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->carrier_fd = 0;
    a->local_session = 0x11111111u;
    a->link_state = LINK_WAIT_HELLO;
    for (i = 0; i < LMUX_CHANNELS; i++) {
        a->ch[i].ep.fd = -1;
        a->ch[i].ep.listen_fd = -1;
    }
}

static void make_frame(struct lmux_frame *fr, uint8_t type, uint8_t ch)
{
    memset(fr, 0, sizeof(*fr));
    fr->version = LMUX_VERSION;
    fr->type = type;
    fr->channel = ch;
}

static void make_hello(struct lmux_frame *fr, uint32_t sid)
{
    make_frame(fr, LMUX_T_HELLO, 0);
    fr->payload_len = 8;
    memcpy(fr->payload, "LMX1", 4);
    lmux_put_le32(fr->payload + 4, sid);
}

static int feed_encoded(struct app *a, const struct lmux_frame *fr)
{
    uint8_t enc[LMUX_MAX_ENCODED];
    size_t enc_len = 0;

    if (lmux_encode_frame(fr, enc, sizeof(enc), &enc_len) < 0)
        return -1;
    process_encoded_record(a, enc, enc_len - 1);
    return 0;
}

static int pop_output_frame(struct app *a, struct lmux_frame *fr)
{
    if (build_next_output(a) <= 0)
        return -1;
    if (a->out_len == 0 || a->out[a->out_len - 1] != 0)
        return -1;
    if (lmux_decode_frame(a->out, a->out_len - 1, fr) < 0)
        return -1;
    a->out_len = 0;
    a->out_pos = 0;
    return 0;
}

static int test_garbage_before_hello(void)
{
    struct app a;
    struct lmux_frame fr;
    const uint8_t junk[] = { 0x01, 0x02, 0x03 };

    init_app(&a);
    process_encoded_record(&a, junk, sizeof(junk));
    if (a.pending_reset || a.link_state != LINK_WAIT_HELLO)
        return fail("garbage before HELLO emitted RESET");

    make_hello(&fr, 0xaabbcc01u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != 0xaabbcc01u)
        return fail("valid HELLO after garbage did not establish");
    return 0;
}

static int test_open_credit_before_hello(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode OPEN");
    make_frame(&fr, LMUX_T_CREDIT, 1);
    fr.payload_len = 2;
    lmux_put_le16(fr.payload, 7);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode CREDIT");
    if (a.pending_reset || a.link_state != LINK_WAIT_HELLO || a.ch[0].remote_open)
        return fail("pre-HELLO OPEN/CREDIT was not ignored");
    return 0;
}

static int test_reset_stale_open_then_hello(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    make_hello(&fr, 0x1000u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    make_frame(&fr, LMUX_T_RESET, 0);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode RESET");
    if (a.link_state != LINK_WAIT_HELLO || a.pending_reset)
        return fail("received RESET cascaded");

    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode stale OPEN");
    if (a.pending_reset || a.ch[0].remote_open)
        return fail("stale OPEN after RESET was not ignored");

    make_hello(&fr, 0x2000u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode second HELLO");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != 0x2000u)
        return fail("HELLO after RESET did not establish");
    return 0;
}

static int test_duplicate_hello(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    a.ch[0].local_open = 1;
    make_hello(&fr, 0x3333u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode OPEN");
    if (!a.ch[0].active)
        return fail("channel did not activate before duplicate HELLO");
    a.ch[0].remote_credit = 5;

    make_hello(&fr, 0x3333u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode duplicate HELLO");
    if (a.link_state != LINK_ESTABLISHED || !a.ch[0].active ||
        a.ch[0].remote_credit != 5 || a.pending_reset)
        return fail("duplicate HELLO reset established channel");
    return 0;
}

static int test_new_session_hello(void)
{
    struct app a;
    struct lmux_frame fr;
    const uint8_t b = 0x5a;

    init_app(&a);
    a.ch[0].local_open = 1;
    make_hello(&fr, 0x4444u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode OPEN");
    ring_write(&a.ch[0].tx, &b, 1);
    ring_write(&a.ch[0].rx, &b, 1);
    a.ch[0].remote_credit = 9;
    a.ch[0].credit_pending = 3;

    make_hello(&fr, 0x5555u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode new HELLO");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != 0x5555u)
        return fail("new-session HELLO did not establish");
    if (a.ch[0].remote_open || a.ch[0].active || a.ch[0].remote_credit ||
        a.ch[0].credit_pending || a.ch[0].tx.len || a.ch[0].rx.len)
        return fail("new-session HELLO did not clear channel transport state");
    if (!a.ch[0].pending_open)
        return fail("new-session HELLO did not re-advertise local OPEN");
    return 0;
}

static int test_empty_cobs_record(void)
{
    struct app a;

    init_app(&a);
    process_encoded_record(&a, NULL, 0);
    if (a.pending_reset || a.link_state != LINK_WAIT_HELLO)
        return fail("empty COBS record was treated as fault");
    return 0;
}

static int test_lost_first_hello_converges(void)
{
    struct app a, b;
    struct lmux_frame fr;

    init_app(&a);
    init_app(&b);
    a.local_session = 0xaaa00001u;
    b.local_session = 0xbbb00001u;
    b.ch[0].local_open = 1;
    b.ch[1].local_open = 1;

    if (pop_output_frame(&b, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("B initial HELLO not generated");
    /* B's first HELLO is intentionally dropped. */

    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("A initial HELLO not generated");
    if (feed_encoded(&b, &fr) < 0)
        return fail("deliver A HELLO to B");
    if (b.link_state != LINK_ESTABLISHED || b.pending_reset)
        return fail("B did not establish after A HELLO");

    if (pop_output_frame(&b, &fr) < 0)
        return fail("B response HELLO not generated");
    if (fr.type != LMUX_T_HELLO)
        return fail("B OPEN overtook HELLO response");
    if (feed_encoded(&a, &fr) < 0)
        return fail("deliver B HELLO response to A");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != b.local_session || a.pending_reset)
        return fail("A did not establish from B HELLO response");

    if (pop_output_frame(&b, &fr) < 0)
        return fail("B OPEN not generated after HELLO response");
    if (fr.type != LMUX_T_OPEN || fr.channel != 1)
        return fail("B first post-HELLO frame was not channel 1 OPEN");
    if (feed_encoded(&a, &fr) < 0)
        return fail("deliver B OPEN to A");
    if (!a.ch[0].remote_open || a.pending_reset)
        return fail("A did not accept B OPEN after handshake");

    return 0;
}

static int test_simultaneous_startup_hello_response_no_loop(void)
{
    struct app a, b;
    struct lmux_frame a_initial, b_initial, fr;

    init_app(&a);
    init_app(&b);
    a.local_session = 0xaaa00002u;
    b.local_session = 0xbbb00002u;

    if (pop_output_frame(&a, &a_initial) < 0 || a_initial.type != LMUX_T_HELLO)
        return fail("A initial simultaneous HELLO not generated");
    if (pop_output_frame(&b, &b_initial) < 0 || b_initial.type != LMUX_T_HELLO)
        return fail("B initial simultaneous HELLO not generated");

    if (feed_encoded(&a, &b_initial) < 0 || feed_encoded(&b, &a_initial) < 0)
        return fail("deliver simultaneous HELLOs");
    if (a.link_state != LINK_ESTABLISHED || b.link_state != LINK_ESTABLISHED)
        return fail("simultaneous HELLOs did not establish both peers");

    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("A HELLO response not generated");
    if (feed_encoded(&b, &fr) < 0)
        return fail("deliver A HELLO response");
    if (pop_output_frame(&b, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("B HELLO response not generated");
    if (feed_encoded(&a, &fr) < 0)
        return fail("deliver B HELLO response");

    if (a.pending_hello || b.pending_hello || a.pending_reset || b.pending_reset)
        return fail("duplicate HELLO responses created loop/reset");
    if (build_next_output(&a) != 0 || build_next_output(&b) != 0)
        return fail("unexpected extra HELLO after duplicate responses");
    return 0;
}

static int test_shell_lifecycle_smoke(void)
{
    struct app a;
    char path[128];
    char link_a[128], link_b[128];
    ssize_t la, lb;
    pid_t first, second;
    int loops;

    init_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-test-shell-%ld", (long)getpid());
    a.ch[0].ep.type = EP_SHELL;
    strcpy(a.ch[0].ep.path, path);
    if (create_pty(&a, 0) < 0)
        return fail("shell PTY create");
    la = readlink(path, link_a, sizeof(link_a) - 1);
    if (la <= 0) {
        cleanup(&a);
        return fail("shell symlink missing");
    }
    link_a[la] = 0;
    spawn_shell(&a, 0);
    first = a.ch[0].ep.child_pid;
    if (first <= 0) {
        cleanup(&a);
        return fail("shell did not spawn");
    }
    kill(first, SIGHUP);
    for (loops = 0; loops < 100 && a.ch[0].ep.child_pid == first; loops++) {
        reap_shells(&a);
        usleep(20000);
    }
    if (a.ch[0].ep.child_pid == first) {
        cleanup(&a);
        return fail("shell was not reaped");
    }
    a.ch[0].ep.respawn_at_ms = 0;
    supervise_shells(&a);
    second = a.ch[0].ep.child_pid;
    if (second <= 0 || second == first) {
        cleanup(&a);
        return fail("shell did not respawn");
    }
    lb = readlink(path, link_b, sizeof(link_b) - 1);
    if (lb <= 0) {
        cleanup(&a);
        return fail("shell symlink disappeared");
    }
    link_b[lb] = 0;
    if (strcmp(link_a, link_b) != 0) {
        cleanup(&a);
        return fail("shell symlink changed across respawn");
    }
    cleanup(&a);
    return 0;
}

static void init_file_app(struct app *a)
{
    init_app(a);
    a->ch[0].ep.type = EP_FILE;
    a->ch[0].local_open = 1;
    a->ch[0].remote_open = 1;
    a->ch[0].active = 1;
    a->ch[0].file.fd = -1;
}

static void feed_lxf(struct app *a, uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t rec_len = 0;
    lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &rec_len);
    ring_write(&a->ch[0].rx, rec, rec_len);
    file_service_step(a, 0);
}

static void feed_lxf_idx(struct app *a, int idx, uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t rec_len = 0;
    lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &rec_len);
    ring_write(&a->ch[idx].rx, rec, rec_len);
    file_service_step(a, idx);
}

static void feed_lxf_buffered(struct app *a, uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t rec_len = 0;
    lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &rec_len);
    ring_write(&a->ch[0].rx, rec, rec_len);
}

static int pop_lxf_idx(struct app *a, int idx, uint8_t *type, uint8_t *payload, uint32_t *len);

static int pop_lxf(struct app *a, uint8_t *type, uint8_t *payload, uint32_t *len)
{
    return pop_lxf_idx(a, 0, type, payload, len);
}

static int pop_lxf_idx(struct app *a, int idx, uint8_t *type, uint8_t *payload, uint32_t *len)
{
    struct channel *c = &a->ch[idx];
    uint8_t rec[LXF_MAX_RECORD];
    uint8_t flags;
    const uint8_t *p;
    size_t n;

    int guard = 0;
    file_service_step(a, idx);
    while (c->tx.len < LXF_HEADER_LEN && guard++ < 64)
        file_service_step(a, idx);
    if (c->tx.len < LXF_HEADER_LEN)
        return -1;
    n = ring_peek_linear(&c->tx, &p);
    if (n >= LXF_HEADER_LEN) {
        memcpy(rec, p, LXF_HEADER_LEN);
        ring_drop(&c->tx, LXF_HEADER_LEN);
    } else {
        memcpy(rec, p, n);
        ring_drop(&c->tx, n);
        memcpy(rec + n, c->tx.buf + c->tx.r, LXF_HEADER_LEN - n);
        ring_drop(&c->tx, LXF_HEADER_LEN - n);
    }
    if (lxf_decode_header(rec, LXF_HEADER_LEN, type, &flags, len) < 0)
        return -1;
    guard = 0;
    while (c->tx.len < *len && guard++ < 64)
        file_service_step(a, idx);
    if (c->tx.len < *len)
        return -1;
    if (*len) {
        n = ring_peek_linear(&c->tx, &p);
        if (n >= *len) {
            memcpy(payload, p, *len);
            ring_drop(&c->tx, *len);
        } else {
            memcpy(payload, p, n);
            ring_drop(&c->tx, n);
            memcpy(payload + n, c->tx.buf + c->tx.r, *len - n);
            ring_drop(&c->tx, *len - n);
        }
    }
    return 0;
}

static void empty_sha(uint8_t digest[32])
{
    struct sha256_ctx sha;
    sha256_init(&sha);
    sha256_final(&sha, digest);
}

static int test_file_zero_put(void)
{
    struct app a;
    char path[128];
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], digest[32], type;
    uint32_t len;
    struct stat st;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-zero-put-%ld", (long)getpid());
    unlink(path);
    lmux_put_le32(p, 0600);
    lmux_put_le64(p + 4, 0);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)p + 16, path);
    feed_lxf(&a, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("zero PUT request not accepted");
    empty_sha(digest);
    lmux_put_le64(p, 0);
    memcpy(p + 8, digest, 32);
    feed_lxf(&a, LXF_T_DONE, p, 40);
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("zero PUT did not complete");
    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != 0) {
        unlink(path);
        return fail("zero PUT file missing");
    }
    unlink(path);
    return 0;
}

static int test_file_bad_hash_preserves_existing(void)
{
    struct app a;
    char path[128];
    int fd;
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    char buf[8] = {0};

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-badhash-%ld", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("create badhash base");
    write(fd, "old", 3);
    close(fd);
    lmux_put_le32(p, 0600);
    lmux_put_le64(p + 4, 3);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)p + 16, path);
    feed_lxf(&a, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("bad hash PUT request not accepted");
    memcpy(p, "new", 3);
    feed_lxf(&a, LXF_T_DATA, p, 3);
    memset(p, 0, 40);
    lmux_put_le64(p, 3);
    feed_lxf(&a, LXF_T_DONE, p, 40);
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_ERROR) {
        unlink(path);
        return fail("bad hash was not rejected");
    }
    fd = open(path, O_RDONLY);
    read(fd, buf, 3);
    close(fd);
    unlink(path);
    if (memcmp(buf, "old", 3) != 0)
        return fail("bad hash replaced existing file");
    return 0;
}

static int test_file_get_roundtrip(void)
{
    struct app a;
    char path[128];
    int fd;
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    int saw_data = 0, saw_done = 0;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-get-%ld", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("create GET source");
    write(fd, "abc", 3);
    close(fd);
    strcpy((char *)p, path);
    feed_lxf(&a, LXF_T_GET_REQUEST, p, (uint32_t)(strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK || lmux_get_le64(out + 4) != 3) {
        unlink(path);
        return fail("GET metadata bad");
    }
    if (pop_lxf(&a, &type, out, &len) == 0 && type == LXF_T_DATA && len == 3 && memcmp(out, "abc", 3) == 0)
        saw_data = 1;
    if (pop_lxf(&a, &type, out, &len) == 0 && type == LXF_T_DONE && lmux_get_le64(out) == 3)
        saw_done = 1;
    unlink(path);
    if (!saw_data || !saw_done)
        return fail("GET data/DONE missing");
    return 0;
}

static int run_buffered_put_size(size_t data_len, int split_done)
{
    struct app a;
    char path[128];
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type, digest[32];
    uint32_t len;
    struct sha256_ctx sha;
    struct stat st;
    size_t i, off;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-buffered-put-%ld-%lu", (long)getpid(), (unsigned long)data_len);
    unlink(path);
    lmux_put_le32(p, 0600);
    lmux_put_le64(p + 4, data_len);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)p + 16, path);
    feed_lxf(&a, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("buffered PUT request not accepted");
    if (a.ch[0].file.state != FS_PUT || a.ch[0].tx.len != 0)
        return fail("buffered PUT request left bad state");

    sha256_init(&sha);
    for (i = 0; i < data_len; i++)
        p[i % sizeof(p)] = (uint8_t)(i * 17 + 3);
    for (off = 0; off < data_len; ) {
        size_t n = data_len - off;
        if (n > 400)
            n = 400;
        for (i = 0; i < n; i++)
            p[i] = (uint8_t)((off + i) * 17 + 3);
        sha256_update(&sha, p, n);
        feed_lxf_buffered(&a, LXF_T_DATA, p, (uint32_t)n);
        if (split_done)
            (void)drain_internal_work(&a);
        off += n;
    }
    sha256_final(&sha, digest);
    lmux_put_le64(p, data_len);
    memcpy(p + 8, digest, 32);
    feed_lxf_buffered(&a, LXF_T_DONE, p, 40);
    if (!drain_internal_work(&a)) {
        unlink(path);
        return fail("buffered PUT produced no internal progress");
    }
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK || len < 40) {
        unlink(path);
        return fail("buffered PUT did not send OK");
    }
    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != data_len) {
        unlink(path);
        return fail("buffered PUT final file bad");
    }
    unlink(path);
    return 0;
}

static int test_file_buffered_data_done(void)
{
    return run_buffered_put_size(32, 0);
}

static int test_file_exact_1134_put(void)
{
    return run_buffered_put_size(1134, 1);
}

static int test_file_no_event_after_done(void)
{
    return run_buffered_put_size(257, 1);
}

static int test_file_get_drain_no_stall(void)
{
    struct app a;
    char path[128];
    int fd;
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    int saw_done = 0, guard;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-get-drain-%ld", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("create GET drain source");
    write(fd, "abcdef", 6);
    close(fd);
    strcpy((char *)p, path);
    feed_lxf_buffered(&a, LXF_T_GET_REQUEST, p, (uint32_t)(strlen(path) + 1));
    for (guard = 0; guard < 16; guard++)
        (void)drain_internal_work(&a);
    while (pop_lxf(&a, &type, out, &len) == 0) {
        if (type == LXF_T_DONE) {
            saw_done = 1;
            break;
        }
        if (a.ch[0].tx.len == 0) {
            for (guard = 0; guard < 16; guard++)
                (void)drain_internal_work(&a);
        }
    }
    unlink(path);
    if (!saw_done)
        return fail("GET drain did not produce DONE");
    return 0;
}

static int spawn_cwd_child(const char *dir)
{
    int pipefd[2];
    char ready;
    pid_t pid;
    if (pipe(pipefd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (chdir(dir) < 0)
            _exit(111);
        ready = 'x';
        (void)write(pipefd[1], &ready, 1);
        close(pipefd[1]);
        for (;;)
            pause();
    }
    close(pipefd[1]);
    if (read(pipefd[0], &ready, 1) != 1) {
        close(pipefd[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    close(pipefd[0]);
    return (int)pid;
}

static void stop_child(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

static void init_cwd_app(struct app *a)
{
    init_app(a);
    a->ch[3].ep.type = EP_FILE;
    a->ch[3].local_open = 1;
    a->ch[3].remote_open = 1;
    a->ch[3].active = 1;
    a->ch[3].file.fd = -1;
}

static int test_cwd_request_shell_channels(void)
{
    struct app a;
    char dir1[128], dir2[128];
    uint8_t req[1], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    pid_t p1 = -1, p2 = -1;
    int rc = 1;

    snprintf(dir1, sizeof(dir1), "/tmp/lmux-cwd1-%ld", (long)getpid());
    snprintf(dir2, sizeof(dir2), "/tmp/lmux-cwd2-%ld", (long)getpid());
    mkdir(dir1, 0700);
    mkdir(dir2, 0700);
    p1 = spawn_cwd_child(dir1);
    p2 = spawn_cwd_child(dir2);
    if (p1 <= 0 || p2 <= 0)
        goto out;

    init_cwd_app(&a);
    a.ch[0].ep.type = EP_SHELL;
    a.ch[0].ep.child_pid = p1;
    a.ch[1].ep.type = EP_SHELL;
    a.ch[1].ep.child_pid = p2;

    req[0] = 1;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_CWD_REPLY ||
        strcmp((char *)out, dir1) != 0)
        goto out;
    req[0] = 2;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_CWD_REPLY ||
        strcmp((char *)out, dir2) != 0)
        goto out;
    rc = 0;
out:
    stop_child(p1);
    stop_child(p2);
    rmdir(dir1);
    rmdir(dir2);
    return rc ? fail("CWD request shell channels") : 0;
}

static int test_cwd_request_errors(void)
{
    struct app a;
    uint8_t req[1], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;

    init_cwd_app(&a);
    req[0] = 3;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_ERROR)
        return fail("CWD invalid channel error");

    init_cwd_app(&a);
    a.ch[0].ep.type = EP_SHELL;
    a.ch[0].ep.child_pid = -1;
    req[0] = 1;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_ERROR)
        return fail("CWD absent shell error");
    return 0;
}

int main(void)
{
    if (test_garbage_before_hello() ||
        test_open_credit_before_hello() ||
        test_reset_stale_open_then_hello() ||
        test_duplicate_hello() ||
        test_new_session_hello() ||
        test_empty_cobs_record() ||
        test_lost_first_hello_converges() ||
        test_simultaneous_startup_hello_response_no_loop() ||
        test_shell_lifecycle_smoke() ||
        test_file_zero_put() ||
        test_file_bad_hash_preserves_existing() ||
        test_file_get_roundtrip() ||
        test_file_buffered_data_done() ||
        test_file_exact_1134_put() ||
        test_file_no_event_after_done() ||
        test_file_get_drain_no_stall() ||
        test_cwd_request_shell_channels() ||
        test_cwd_request_errors())
        return 1;
    puts("lmuxd state selftest ok");
    return 0;
}
