#!/usr/bin/env bash
# Launch bench/cenpack_job.sh with the v3 tree's diff embedded.  BASE is the last
# commit whose src/include write v3 wefts (default: the merge-base with main).
set -euo pipefail
cd "$(dirname "$0")/.."
BASE=${BASE:-$(git merge-base HEAD main)}
job=/scratch/pg_weave/cenpack_job_$(git rev-parse --short HEAD).sh
{
	echo '#!/usr/bin/env bash'
	echo 'write_v3_patch() {'
	echo "cat <<'__V3_PATCH__'"
	git diff HEAD "$BASE" -- src include
	echo '__V3_PATCH__'
	echo '}'
	tail -n +2 bench/cenpack_job.sh
} > "$job"
bash -n "$job"
echo "job: $job (v3 = $BASE)"
SCRIPT=$job exec bench/aws/run.sh "${1:-c7i.4xlarge}" script
