#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/mdio.h>

#include "mdio.h"

#define MVLS_CMD  0
#define MVLS_CMD_BUSY BIT(15)
#define MVLS_CMD_C22  BIT(12)

#define MVLS_DATA 1

#define MVLS_G1 0x1b
#define MVLS_G2 0x1c

#define MVLS_REG(_port, _reg) (((_port) << 16) | (_reg))

enum mvls_famliy {
	FAM_UNKNOWN,
	FAM_SPINNAKER,
	FAM_OPAL,
	FAM_AGATE,
	FAM_PERIDOT,
	FAM_AMETHYST,
};

struct mvls_device {
	struct mdio_device dev;
	uint16_t id;

};

static inline uint16_t mvls_multi_cmd(uint8_t port, uint8_t reg, bool write)
{
	return MVLS_CMD_BUSY | MVLS_CMD_C22 | ((write ? 1 : 2) << 10)|
		(port << 5) | reg;
}

static void mvls_wait_cmd(struct mdio_prog *prog, uint8_t id)
{
	int retry;

	retry = prog->len;
	mdio_prog_push(prog, INSN(READ, IMM(id), IMM(MVLS_CMD), REG(0)));
	mdio_prog_push(prog, INSN(AND, REG(0), IMM(MVLS_CMD_BUSY), REG(0)));
	mdio_prog_push(prog, INSN(JEQ, REG(0), IMM(MVLS_CMD_BUSY),
				  GOTO(prog->len, retry)));
}

static void mvls_read_to(struct mdio_device *dev, struct mdio_prog *prog,
			 uint32_t reg, uint8_t to)
{
	struct mvls_device *mdev = (void *)dev;
	uint16_t port;

	port = reg >> 16;
	reg &= 0xffff;

	if (!mdev->id) {
		/* Single-chip addressing, the switch uses the entire
		 * underlying bus */
		mdio_prog_push(prog, INSN(READ, IMM(port), IMM(reg), REG(to)));
		return;
	}

	mdio_prog_push(prog, INSN(WRITE, IMM(mdev->id), IMM(MVLS_CMD),
				  IMM(mvls_multi_cmd(port, reg, false))));
	mvls_wait_cmd(prog, mdev->id);
	mdio_prog_push(prog, INSN(READ, IMM(mdev->id), IMM(MVLS_DATA), REG(to)));
}

static int mvls_read(struct mdio_device *dev, struct mdio_prog *prog,
		     uint32_t reg)
{
	mvls_read_to(dev, prog, reg, 0);
	return 0;
}

static int mvls_write(struct mdio_device *dev, struct mdio_prog *prog,
		      uint32_t reg, uint32_t val)
{
	struct mvls_device *mdev = (void *)dev;
	uint16_t port;

	port = reg >> 16;
	reg &= 0xffff;

	if (!mdev->id) {
		/* Single-chip addressing, the switch uses the entire
		 * underlying bus */
		mdio_prog_push(prog, INSN(WRITE, IMM(port), IMM(reg), val));
		return 0;
	}

	mdio_prog_push(prog, INSN(WRITE, IMM(mdev->id), IMM(MVLS_DATA), val));
	mdio_prog_push(prog, INSN(WRITE, IMM(mdev->id), IMM(MVLS_CMD),
				  IMM(mvls_multi_cmd(port, reg, true))));
	mvls_wait_cmd(prog, mdev->id);
	return 0;
}

static void mvls_wait(struct mdio_device *dev, struct mdio_prog *prog,
		     uint32_t reg)
{
	int retry = prog->len;

	mvls_read_to(dev, prog, reg, 0);
	mdio_prog_push(prog, INSN(AND, REG(0), IMM(MVLS_CMD_BUSY), REG(0)));
	mdio_prog_push(prog, INSN(JEQ, REG(0), IMM(MVLS_CMD_BUSY),
				  GOTO(prog->len, retry)));
}

int mvls_id_cb(uint32_t *data, int len, int err, void *_id)
{
	uint32_t *id = _id;

	if (len != 1)
		return 1;

	*id = *data;
	return err;
}

static uint32_t mvls_id_exec(struct mdio_device *dev)
{
	struct mdio_prog prog = MDIO_PROG_EMPTY;
	uint32_t id;
	int err;

	mvls_read(dev, &prog, MVLS_REG(0x10, 0x03));
	mdio_prog_push(&prog, INSN(EMIT, REG(0), 0, 0));

	err = mdio_xfer(dev->bus, &prog, mvls_id_cb, &id);
	free(prog.insns);
	if (err) {
		fprintf(stderr, "ERROR: ID operation failed (%d)\n", err);
		return 0;
	}

	return id;
}

static enum mvls_famliy mvls_get_family(struct mdio_device *dev)
{
	uint32_t id = mvls_id_exec(dev);

	switch (id >> 4) {
	case 0x099:
		return FAM_OPAL;
	case 0x352:
		return FAM_AGATE;
	case 0x0a1:
		return FAM_PERIDOT;
	default:
		break;
	}

	return FAM_UNKNOWN;
}

static void mvls_print_portvec(uint16_t portvec)
{
	int i;

	for (i = 0; i < 11; i++) {
		if (portvec & (1 << i))
			printf("  %x", i);
		else
			fputs("  .", stdout);
	}

}

int mvls_lag_cb(uint32_t *data, int len, int err, void *_null)
{
	int mask, lag;

	if (len != 16 + 8)
		return 1;

	puts("\e[7m LAG  0  1  2  3  4  5  6  7  8  9  a\e[0m");
	for (lag = 0; lag < 16; lag++) {
		if (!(data[lag] & 0x7ff))
			continue;

		printf("%4d", lag);
		mvls_print_portvec(data[lag]);
		putchar('\n');
	}

	putchar('\n');
	data += 16;

	puts("\e[7mMASK  0  1  2  3  4  5  6  7  8  9  a\e[0m");
	for (mask = 0; mask < 8; mask++) {
		printf("%4d", mask);
		mvls_print_portvec(data[mask]);
		putchar('\n');
	}
	return err;
}

static int mvls_lag_exec(struct mdio_device *dev, int argc, char **argv)
{
	struct mdio_prog prog = MDIO_PROG_EMPTY;
	char *arg;
	int err, i;

	/* Drop "lag" token. */
	argv_pop(&argc, &argv);

	arg = argv_pop(&argc, &argv);
	if (arg) {
		fprintf(stderr, "ERROR: Unknown LAG command\n");
		return 1;
	}

	for (i = 0; i < 16; i++) {
		mvls_write(dev, &prog, MVLS_REG(MVLS_G2, 0x08), IMM(i << 11));
		mvls_read(dev, &prog, MVLS_REG(MVLS_G2, 0x08));
		mdio_prog_push(&prog, INSN(EMIT, REG(0), 0, 0));
	}

	for (i = 0; i < 8; i++) {
		mvls_read(dev, &prog, MVLS_REG(MVLS_G2, 0x07));

		/* Keep the current value of the HashTrunk bit when
		 * selecting the mask to read out. */
		mdio_prog_push(&prog, INSN(AND, REG(0), IMM(1 << 11), REG(0)));
		mdio_prog_push(&prog, INSN(OR, REG(0), IMM(i << 12), REG(0)));

		mvls_write(dev, &prog, MVLS_REG(MVLS_G2, 0x07), REG(0));
		mvls_read(dev, &prog, MVLS_REG(MVLS_G2, 0x07));
		mdio_prog_push(&prog, INSN(EMIT, REG(0), 0, 0));
	}

	err = mdio_xfer(dev->bus, &prog, mvls_lag_cb, NULL);
	free(prog.insns);
	if (err) {
		fprintf(stderr, "ERROR: LAG operation failed (%d)\n", err);
		return 1;
	}

	return 0;
}

struct mvls_counter_ctx {
	enum mvls_famliy fam;

	uint32_t prev[11][6];
};

int mvls_counter_cb(uint32_t *data, int len, int err, void *_ctx)
{
	struct mvls_counter_ctx *ctx = _ctx;
	uint32_t now[6];
	int i, n;

	if (len != 11 * 6 * 2)
		return 1;

	printf("    \e[7m Bcasts   Ucasts   Mcasts\e[0m\n");
	printf("\e[7mP    Rx  Tx   Rx  Tx   Rx  Tx\e[0m\n");

	for (i = 0; i < 11; i++, data += 6 * 2) {
		for (n = 0; n < 6; n++)
			now[n] = (data[2 * n] << 16) | data[2 * n + 1];

		if (!memcmp(ctx->prev[i], now, sizeof(now)))
			continue;

		printf("%x   %3u %3u  %3u %3u  %3u %3u\n", i,
		       now[1] - ctx->prev[i][1], now[4] - ctx->prev[i][4],
		       now[0] - ctx->prev[i][0], now[3] - ctx->prev[i][3],
		       now[2] - ctx->prev[i][2], now[5] - ctx->prev[i][5]);

		memcpy(ctx->prev[i], now, sizeof(now));
	}
	return err;
}

static void mvls_counter_read_one(struct mdio_device *dev, struct mdio_prog *prog,
				  uint8_t counter)
{
	mvls_write(dev, prog, MVLS_REG(MVLS_G1, 0x1d), IMM((1 << 15) | (4 << 12) | counter));
	mvls_wait(dev, prog, MVLS_REG(MVLS_G1, 0x1d));

	mvls_read(dev, prog, MVLS_REG(MVLS_G1, 0x1e));
	mdio_prog_push(prog, INSN(EMIT, REG(0), 0, 0));
	mvls_read(dev, prog, MVLS_REG(MVLS_G1, 0x1f));
	mdio_prog_push(prog, INSN(EMIT, REG(0), 0, 0));

}

static int mvls_counter_exec(struct mdio_device *dev, int argc, char **argv)
{
	struct mdio_prog prog = MDIO_PROG_EMPTY;
	struct mvls_counter_ctx ctx = {};
	int err, base, shift, loop;
	bool repeat = false;
	char *arg;

	/* Drop "counter" token. */
	argv_pop(&argc, &argv);

	arg = argv_pop(&argc, &argv);
	if (arg) {
		if (!strcmp(arg, "repeat"))
			repeat = true;
		else {
			fprintf(stderr, "ERROR: Unexpected counter command\n");
			return 1;
		}
	}

	ctx.fam = mvls_get_family(dev);
	switch (ctx.fam) {
	case FAM_AGATE:
	case FAM_PERIDOT:
	case FAM_AMETHYST:
		base = 1 << 5;
		shift = 1 << 5;
		break;
	default:
		base = 0;
		shift = 1;
	}

	mvls_wait(dev, &prog, MVLS_REG(MVLS_G1, 0x1d));

	mdio_prog_push(&prog, INSN(ADD, IMM((1 << 15) | (5 << 12) | base), IMM(0), REG(1)));

	loop = prog.len;

	mvls_write(dev, &prog, MVLS_REG(MVLS_G1, 0x1d), REG(1));
	mvls_wait(dev, &prog, MVLS_REG(MVLS_G1, 0x1d));

	mvls_counter_read_one(dev, &prog, 0x04);
	mvls_counter_read_one(dev, &prog, 0x06);
	mvls_counter_read_one(dev, &prog, 0x07);

	mvls_counter_read_one(dev, &prog, 0x10);
	mvls_counter_read_one(dev, &prog, 0x13);
	mvls_counter_read_one(dev, &prog, 0x12);

	mdio_prog_push(&prog, INSN(ADD, REG(1), IMM(shift), REG(1)));
	mdio_prog_push(&prog, INSN(JNE, REG(1),
				   IMM((1 << 15) | (5 << 12) | (base + (shift * 11))),
				   GOTO(prog.len, loop)));

	while (!(err = mdio_xfer(dev->bus, &prog, mvls_counter_cb, &ctx))) {
		if (repeat) {
			sleep(1);
			fputs("\e[2J", stdout);
		} else {
			break;
		}
	}

	free(prog.insns);
	if (err) {
		fprintf(stderr, "ERROR: COUNTER operation failed (%d)\n", err);
		return 1;
	}

	return 0;
}

int mvls_atu_cb(uint32_t *data, int len, int err, void *_null)
{
	if (len != 0)
		return 1;

	return err;
}

static int mvls_atu_exec(struct mdio_device *dev, int argc, char **argv)
{
	struct mdio_prog prog = MDIO_PROG_EMPTY;
	uint8_t op = 0;
	char *arg;
	int err;

	/* Drop "atu" token. */
	argv_pop(&argc, &argv);

	arg = argv_pop(&argc, &argv);
	if (!arg) {
		fprintf(stderr, "ERROR: Expected ATU command\n");
		return 1;
	} else if (!strcmp(arg, "flush")) {
		arg = argv_pop(&argc, &argv);
		if (!arg) {
			op += 2;
			goto exec;
		} else if (!strcmp(arg, "all")) {
			goto read_stat;
		} else {
			long fid = strtol(arg, NULL, 0);

			if (fid < 0) {
				fprintf(stderr, "ERROR: Invalid FID \"%s\"\n", arg);
				return 1;
			}

			/* Limit to specific FID */
			mvls_read_to(dev, &prog, MVLS_REG(MVLS_G1, 0x01), 0);
			mdio_prog_push(&prog, INSN(AND, REG(0), IMM(0xf0000), REG(0)));
			mdio_prog_push(&prog, INSN(OR, REG(0), IMM(fid & 0xfff), REG(0)));
			mvls_write(dev, &prog, MVLS_REG(MVLS_G1, 0x01), REG(0));
			op = 4;
		}

	read_stat:
		arg = argv_pop(&argc, &argv);
		if (!arg) {
			op += 2;
		} else if (!strcmp(arg, "static")) {
			op += 1;
		} else {
			fprintf(stderr, "ERROR: Invalid option \"%s\"\n", arg);
		}
	} else {
		fprintf(stderr, "ERROR: Unknown ATU command \"%s\"\n", arg);
		return 1;
	}

exec:
	mvls_wait(dev, &prog, MVLS_REG(MVLS_G1, 0x0b));

	mvls_read_to(dev, &prog, MVLS_REG(MVLS_G1, 0x0b), 0);
	mdio_prog_push(&prog, INSN(AND, REG(0), IMM(0xfff), REG(0)));
	mdio_prog_push(&prog, INSN(OR, REG(0), IMM(BIT(15) | (op << 12)), REG(0)));
	mvls_write(dev, &prog, MVLS_REG(MVLS_G1, 0x0b), REG(0));

	mvls_wait(dev, &prog, MVLS_REG(MVLS_G1, 0x0b));

	err = mdio_xfer(dev->bus, &prog, mvls_atu_cb, NULL);
	free(prog.insns);
	if (err) {
		fprintf(stderr, "ERROR: ATU operation failed (%d)\n", err);
		return 1;
	}

	return 0;
}

static int mvls_parse_reg(struct mdio_device *dev, int *argcp, char ***argvp,
			  uint32_t *regs, uint32_t *rege)
{
	char *str, *tok, *end;
	unsigned long r;
	uint16_t port;

	if (rege) {
		fprintf(stderr, "ERROR: Implement ranges\n");
		return ENOSYS;
	}

	str = argv_pop(argcp, argvp);
	tok = str ? strtok(str, ":") : NULL;
	if (!tok) {
		fprintf(stderr, "ERROR: Expected PORT:REG\n");
		return EINVAL;
	}

	if (!strcmp(tok, "global1") || !strcmp(tok, "g1")) {
		port = MVLS_G1;
	} else if (!strcmp(tok, "global2") || !strcmp(tok, "g2")) {
		port = MVLS_G2;
	} else {
		r = strtoul(tok, &end, 0);
		if (*end) {
			fprintf(stderr, "ERROR: \"%s\" is not a valid port\n", tok);
			return EINVAL;
		}

		if (r > 31) {
			fprintf(stderr, "ERROR: Port %lu is out of range [0-31]\n", r);
			return EINVAL;
		}

		port = r;
	}

	tok = strtok(NULL, ":");
	if (!tok) {
		fprintf(stderr, "ERROR: Expected REG\n");
		return EINVAL;
	}

	r = strtoul(tok, &end, 0);
	if (*end) {
		fprintf(stderr, "ERROR: \"%s\" is not a valid register\n",
			tok);
		return EINVAL;
	}

	if (r > 31) {
		fprintf(stderr, "ERROR: register %lu is out of range [0-31]\n", r);
		return EINVAL;
	}

	*regs = (port << 16) | r;
	return 0;
}

struct mdio_driver mvls_driver = {
	.read = mvls_read,
	.write = mvls_write,

	.parse_reg = mvls_parse_reg,
};

static int mvls_exec(const char *bus, int argc, char **argv)
{
	struct mvls_device mdev = {
		.dev = {
			.bus = bus,
			.driver = &mvls_driver,

			.mem = {
				.stride = 1,
				.width = 16,
			},
		},
	};
	char *arg;

	arg = argv_pop(&argc, &argv);
	if (!arg || mdio_parse_dev(arg, &mdev.id, true))
		return 1;

	arg = argv_peek(argc, argv);
	if (!arg)
		return 1;

	if (!strcmp(arg, "atu"))
		return mvls_atu_exec(&mdev.dev, argc, argv);
	if (!strcmp(arg, "counter"))
		return mvls_counter_exec(&mdev.dev, argc, argv);
	if (!strcmp(arg, "lag"))
		return mvls_lag_exec(&mdev.dev, argc, argv);

	return mdio_common_exec(&mdev.dev, argc, argv);
}
DEFINE_CMD("mvls", mvls_exec);
