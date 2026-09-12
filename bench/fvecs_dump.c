/*
 * bench/fvecs_dump.c -- turn a TEXMEX .fvecs file into COPY-ready TSV.
 *
 * One loader, two consumers: the text output format `[v1,v2,...]` is accepted
 * verbatim by both pg_weave's `wvec` and pgvector's `vector`, so the same corpus
 * file loads into either engine without a per-engine converter that could
 * introduce a difference between them. That matters because the two are being
 * compared: a loader bug that touched only one side would look like a result.
 *
 * Normalization is ON by default and is not cosmetic. bench/ivf_recall.c scores
 * inner product on L2-normalized rows (= cosine), so any baseline that is to be
 * compared against its recall numbers must see the same geometry. TEXMEX GIST
 * descriptors are not unit norm, which also means the ground truth shipped with
 * the corpus (gist_groundtruth.ivecs, exact L2 on RAW vectors) does NOT apply
 * once rows are normalized -- ranking under L2 and under cosine agree only on
 * unit-norm data. Ground truth therefore has to be recomputed downstream rather
 * than read from the file, and that is a feature: an exact sequential scan on
 * the same table is a stronger reference than a file that might describe a
 * different preprocessing.
 *
 * Zero-norm rows are dropped, not emitted as zeros: weave_encode() refuses them
 * (bench/ivf_recall.c hits the same case -- GIST's first 100k rows contain one),
 * and a silently normalized 0/0 row would be a NaN that poisons every distance.
 * The count goes to stderr so the row total can be reconciled.
 *
 * usage: fvecs_dump <file.fvecs> <nrows> [skip] [raw]
 *          skip  rows to skip first, for holding out a query set
 *          raw   pass to disable normalization
 * stdout: id<TAB>[v1,...,vd]   ids are 1-based and consecutive over EMITTED rows
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <file.fvecs> <nrows> [skip] [raw]\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];
	long		nrows = atol(argv[2]);
	long		skip = argc > 3 ? atol(argv[3]) : 0;
	int			norm = !(argc > 4 && strcmp(argv[4], "raw") == 0);

	FILE	   *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}

	int32_t		dim = 0;
	if (fread(&dim, 4, 1, f) != 1 || dim <= 0 || dim > 65536) {
		fprintf(stderr, "%s: bad fvecs header (dim=%d)\n", path, dim);
		return 1;
	}
	rewind(f);

	size_t		reclen = 4 + (size_t) dim * 4;
	float	   *v = malloc((size_t) dim * sizeof(float));
	if (!v)
		return 1;

	if (skip > 0 && fseek(f, (long) (skip * reclen), SEEK_SET) != 0) {
		perror("fseek");
		return 1;
	}

	/*
	 * A 960-d row is ~9 KB of text and a 1M-row dump is ~9 GB, so the output
	 * path is worth not making slow: one big stdio buffer, and the float
	 * formatter is %.9g rather than a printf of every digit. %.9g round-trips a
	 * float32 exactly, which is required -- the whole point is that the engine
	 * under test sees the same bits the recall harness saw.
	 */
	static char outbuf[1 << 22];
	setvbuf(stdout, outbuf, _IOFBF, sizeof(outbuf));

	long		emitted = 0, dropped = 0;
	for (long i = 0; i < nrows; i++) {
		int32_t		d;

		if (fread(&d, 4, 1, f) != 1)
			break;
		if (d != dim) {
			fprintf(stderr, "row %ld: dim %d != %d\n", i, d, dim);
			return 1;
		}
		if (fread(v, 4, (size_t) dim, f) != (size_t) dim)
			break;

		if (norm) {
			double		ss = 0.0;

			for (int j = 0; j < dim; j++)
				ss += (double) v[j] * (double) v[j];
			if (ss <= 0.0) {
				dropped++;
				continue;
			}
			double		inv = 1.0 / sqrt(ss);

			for (int j = 0; j < dim; j++)
				v[j] = (float) ((double) v[j] * inv);
		}

		printf("%ld\t[", ++emitted);
		for (int j = 0; j < dim; j++)
			printf(j ? ",%.9g" : "%.9g", (double) v[j]);
		fputs("]\n", stdout);
	}
	fflush(stdout);
	fprintf(stderr, "dim=%d emitted=%ld dropped_zero_norm=%ld normalized=%s\n",
			dim, emitted, dropped, norm ? "yes" : "no");
	return 0;
}
