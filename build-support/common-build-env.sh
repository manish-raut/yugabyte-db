#@IgnoreInspection BashAddShebang

# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

# This is common between build and test scripts.

set -euo pipefail

if [[ $BASH_SOURCE == $0 ]]; then
  echo "$BASH_SOURCE must be sourced, not executed" >&2
  exit 1
fi

# Guard against multiple inclusions.
if [[ -n ${YB_COMMON_BUILD_ENV_SOURCED:-} ]]; then
  # Return to the executing script.
  return
fi

YB_COMMON_BUILD_ENV_SOURCED=1

# -------------------------------------------------------------------------------------------------
# Load yugabyte-bash-common
# -------------------------------------------------------------------------------------------------

set_yb_src_root() {
  export YB_SRC_ROOT=$1
  YB_BUILD_SUPPORT_DIR=$YB_SRC_ROOT/build-support
  if [[ ! -d $YB_SRC_ROOT ]]; then
    fatal "YB_SRC_ROOT directory '$YB_SRC_ROOT' does not exist"
  fi
  YB_COMPILER_WRAPPER_CC=$YB_BUILD_SUPPORT_DIR/compiler-wrappers/cc
  YB_COMPILER_WRAPPER_CXX=$YB_BUILD_SUPPORT_DIR/compiler-wrappers/c++
  yb_java_project_dirs=( "$YB_SRC_ROOT/java" "$YB_SRC_ROOT/ent/java" )
}

# This script is expected to be in build-support, a subdirectory of the repository root directory.
set_yb_src_root "$( cd "$( dirname "$BASH_SOURCE" )"/.. && pwd )"

if [[ $YB_SRC_ROOT == */ ]]; then
  fatal "YB_SRC_ROOT ends with '/' (not allowed): '$YB_SRC_ROOT'"
fi

YB_BASH_COMMON_DIR=$YB_SRC_ROOT/submodules/yugabyte-bash-common
if [[ ! -d $YB_BASH_COMMON_DIR || -z "$( ls -A "$YB_BASH_COMMON_DIR" )" ]]; then
  ( cd "$YB_SRC_ROOT"; git submodule update --init --recursive )
fi

. "$YB_SRC_ROOT/submodules/yugabyte-bash-common/src/yugabyte-bash-common.sh"

# -------------------------------------------------------------------------------------------------
# Constants
# -------------------------------------------------------------------------------------------------

declare -i MAX_JAVA_BUILD_ATTEMPTS=5

# Reuse the C errno value for this.
declare -r -i YB_EXIT_CODE_NO_SUCH_FILE_OR_DIRECTORY=2

# What matches these expressions will be filtered out of Maven output.
MVN_OUTPUT_FILTER_REGEX='\[INFO\] (Download(ing|ed): '
MVN_OUTPUT_FILTER_REGEX+='|[^ ]+ already added, skipping$)'
MVN_OUTPUT_FILTER_REGEX+='|^Generating .*[.]html[.][.][.]$'
MVN_OUTPUT_FILTER_REGEX+='|^\[INFO\] Copying .*[.]jar to .*[.]jar$'

readonly YB_JENKINS_NFS_HOME_DIR=/n/jenkins

# In our NFS environment, we keep Linuxbrew builds in this directory.
readonly SHARED_LINUXBREW_BUILDS_DIR="$YB_JENKINS_NFS_HOME_DIR/linuxbrew"
readonly SHARED_CUSTOM_HOMEBREW_BUILDS_DIR="$YB_JENKINS_NFS_HOME_DIR/homebrew"

# We look for the list of distributed build worker nodes in this file. This gets populated by
# a cronjob on buildmaster running under the jenkins user (as of 06/20/2017).
YB_BUILD_WORKERS_FILE=${YB_BUILD_WORKERS_FILE:-$YB_JENKINS_NFS_HOME_DIR/run/build-workers}

# The assumed number of cores per build worker. This is used in the default make parallelism level
# calculation in yb_build.sh. This does not have to be the exact number of cores per worker, but
# will affect whether or not we force the auto-scaling group of workers to expand.
readonly YB_NUM_CORES_PER_BUILD_WORKER=8

# The "number of build workers" that we'll end up using to compute the parallelism (by multiplying
# it by YB_NUM_CORES_PER_BUILD_WORKER) will be first brought into this range.
readonly MIN_EFFECTIVE_NUM_BUILD_WORKERS=5
readonly MAX_EFFECTIVE_NUM_BUILD_WORKERS=10

readonly MVN_OUTPUT_FILTER_REGEX

# An even faster alternative to downloading a pre-built third-party dependency tarball from S3
# or Google Storage: just use a pre-existing third-party build from NFS. This has to be maintained
# outside of main (non-thirdparty) YB codebase's build pipeline.
readonly NFS_PARENT_DIR_FOR_SHARED_THIRDPARTY="$YB_JENKINS_NFS_HOME_DIR/thirdparty"

# We create a Python Virtual Environment inside this directory in the build directory.
readonly YB_VIRTUALENV_BASENAME=python_virtual_env

readonly YB_LINUXBREW_LOCAL_ROOT=$HOME/.linuxbrew-yb-build

readonly YB_SHARED_MVN_LOCAL_REPO="$YB_JENKINS_NFS_HOME_DIR/m2_repository"
readonly YB_NON_SHARED_MVN_LOCAL_REPO=$HOME/.m2/repository
readonly YB_SHARED_MVN_SETTINGS="$YB_JENKINS_NFS_HOME_DIR/m2_settings.xml"

if [[ -z ${is_run_test_script:-} ]]; then
  is_run_test_script=false
fi
readonly is_run_test_script

# Setting this to "true" will prevent any changes to the virtualenv (creating it or installing
# modules into it) as part of activate_virtualenv.
yb_readonly_virtualenv=false

# How long we'll wait while a concurrent process downloads and extracts a third-party archive.
declare -i -r YB_DEP_DOWNLOAD_LOCK_WAIT_SEC=120

YB_NFS_DOWNLOAD_CACHE_DIR=${YB_NFS_DOWNLOAD_CACHE_DIR:-$YB_JENKINS_NFS_HOME_DIR/download_cache}

readonly VALID_BUILD_TYPES=(
  asan
  debug
  fastdebug
  idebug
  irelease
  ifastdebug
  profile_build
  profile_gen
  release
  tsan
  tsan_slow
  compilecmds
)

# Valid values of CMAKE_BUILD_TYPE passed to the top-level CMake build. This is the same as the
# above with the exclusion of ASAN/TSAN.
readonly VALID_CMAKE_BUILD_TYPES=(
  debug
  fastdebug
  profile_build
  profile_gen
  release
)

readonly VALID_COMPILER_TYPES=( gcc clang zapcc gcc8 )

readonly VALID_LINKING_TYPES=( static dynamic )

make_regexes_from_lists \
  VALID_BUILD_TYPES \
  VALID_CMAKE_BUILD_TYPES \
  VALID_COMPILER_TYPES \
  VALID_LINKING_TYPES

readonly BUILD_ROOT_BASENAME_RE=\
"^($VALID_BUILD_TYPES_RAW_RE)-\
($VALID_COMPILER_TYPES_RAW_RE)-\
($VALID_LINKING_TYPES_RAW_RE)\
(-ninja)?\
(-clion)?$"

# We first use this to find ephemeral drives.
readonly EPHEMERAL_DRIVES_GLOB="/mnt/ephemeral* /mnt/d*"

# We then filter the drives found using this.
# The way we use this regex we expect it NOT to be anchored in the end.
readonly EPHEMERAL_DRIVES_FILTER_REGEX="^/mnt/(ephemeral|d)[0-9]+"  # No "$" in the end.

declare -i -r DIRECTORY_EXISTENCE_WAIT_TIMEOUT_SEC=100

declare -i -r YB_DOWNLOAD_LOCK_TIMEOUT_SEC=120

readonly YB_DOWNLOAD_LOCKS_DIR=/tmp/yb_download_locks

readonly YB_NFS_PATH_RE="^/(n|z|u|net|Volumes/net)/"

# -------------------------------------------------------------------------------------------------
# Functions
# -------------------------------------------------------------------------------------------------

is_thirdparty_build() {
  [[ ${YB_IS_THIRDPARTY_BUILD:-0} == "1" ]]
}

normalize_build_type() {
  if [[ -z ${build_type:-} ]]; then
    if [[ -n ${BUILD_TYPE:-} ]]; then
      build_type=$BUILD_TYPE
    else
      fatal "Neither build_type or BUILD_TYPE are set"
    fi
  fi
  validate_build_type "$build_type"
  local lowercase_build_type=$( echo "$build_type" | to_lowercase )
  if [[ "$build_type" != "$lowercase_build_type" ]]; then
    # Only assign if we actually need to, because the build_type variable may already be read-only.
    build_type=$lowercase_build_type
  fi
}

# Sets the build directory based on the given build type (the build_type variable) and the value of
# the YB_COMPILER_TYPE environment variable.
set_build_root() {
  set_use_ninja
  if [[ ${1:-} == "--no-readonly" ]]; then
    local -r make_build_root_readonly=false
    shift
  else
    local -r make_build_root_readonly=true
  fi

  expect_num_args 0 "$@"
  normalize_build_type
  readonly build_type

  if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    fatal "YB_COMPILER_TYPE is not set"
  fi
  validate_compiler_type "$YB_COMPILER_TYPE"
  determine_linking_type

  BUILD_ROOT=$YB_BUILD_PARENT_DIR/$build_type-$YB_COMPILER_TYPE-$YB_LINK

  if using_ninja; then
    BUILD_ROOT+="-ninja"
  fi

  normalize_build_root

  if "$make_build_root_readonly"; then
    readonly BUILD_ROOT
  fi

  if [[ -n ${predefined_build_root:-} && $predefined_build_root != $BUILD_ROOT ]] &&
     ! "$YB_BUILD_SUPPORT_DIR/is_same_path.py" "$predefined_build_root" "$BUILD_ROOT"; then
    fatal "An inconsistency between predefined BUILD_ROOT ('$predefined_build_root') and" \
          "computed BUILD_ROOT ('$BUILD_ROOT')."
  fi

  export BUILD_ROOT
  export YB_BUILD_ROOT=$BUILD_ROOT
}

# Resolve the BUILD_ROOT symlink and save the result to the real_build_root_path variable.
set_real_build_root_path() {
  if [[ -h $BUILD_ROOT ]]; then
    real_build_root_path=$( readlink "$BUILD_ROOT" )
  else
    real_build_root_path="$BUILD_ROOT"
  fi

  readonly real_build_root_path=$( cd "$real_build_root_path" && pwd )
}

ensure_build_root_is_set() {
  if [[ -z ${BUILD_ROOT:-} ]]; then
    fatal "The BUILD_ROOT environment variable is not set. This must point to the absolute path" \
          "of the build root directory, e.g. '<yugabyte_src_dir>/build/debug'."
  fi
}

# TODO: rename to ensure_build_root_exists
ensure_build_root_exists() {
  ensure_build_root_is_set
  if [[ ! -d $BUILD_ROOT ]]; then
    fatal "The directory BUILD_ROOT ('$BUILD_ROOT') does not exist"
  fi
}

normalize_build_root() {
  ensure_build_root_is_set
  if [[ -d $BUILD_ROOT ]]; then
    BUILD_ROOT=$( cd "$BUILD_ROOT" && pwd )
  fi
}

determine_linking_type() {
  if [[ -z "${YB_LINK:-}" ]]; then
    YB_LINK=dynamic
  fi
  if [[ ! "${YB_LINK:-}" =~ ^$VALID_LINKING_TYPES_RE$ ]]; then
    fatal "Expected YB_LINK to be set to \"static\" or \"dynamic\", got \"${YB_LINK:-}\""
  fi
  export YB_LINK
  readonly YB_LINK
}

validate_build_type() {
  expect_num_args 1 "$@"
  # Local variable named _build_type to avoid a collision with the global build_type variable.
  local _build_type=$1
  if ! is_valid_build_type "$_build_type"; then
    fatal "Invalid build type: '$_build_type'. Valid build types are: ${VALID_BUILD_TYPES[@]}" \
          "(case-insensitive)."
  fi
}

is_valid_build_type() {
  expect_num_args 1 "$@"
  local -r _build_type=$( echo "$1" | to_lowercase )
  [[ "$_build_type" =~ $VALID_BUILD_TYPES_RE ]]
}

set_build_type_based_on_jenkins_job_name() {
  if [[ -n "${build_type:-}" ]]; then
    if [[ -n "${JOB_NAME:-}" ]]; then
      # This message only makes sense if JOB_NAME is set.
      log "Build type is already set to '$build_type', not setting it based on Jenkins job name."
    fi
    normalize_build_type
    readonly build_type
    return
  fi

  build_type=debug
  if [[ -z "${JOB_NAME:-}" ]]; then
    log "Using build type '$build_type' by default because JOB_NAME is not set."
    readonly build_type
    return
  fi
  local _build_type  # to avoid collision with the global build_type variable
  local jenkins_job_name=$( echo "$JOB_NAME" | to_lowercase )
  for _build_type in "${VALID_BUILD_TYPES[@]}"; do
    if [[ "-$jenkins_job_name-" =~ [-_]$_build_type[-_] ]]; then
      log "Using build type '$_build_type' based on Jenkins job name '$JOB_NAME'."
      readonly build_type=$_build_type
      return
    fi
  done
  readonly build_type
  log "Using build type '$build_type' by default: could not determine from Jenkins job name" \
      "'$JOB_NAME'."
}

set_default_compiler_type() {
  if [[ -z "${YB_COMPILER_TYPE:-}" ]]; then
    if [[ "$OSTYPE" =~ ^darwin ]]; then
      YB_COMPILER_TYPE=clang
    else
      YB_COMPILER_TYPE=gcc
    fi
    export YB_COMPILER_TYPE
    readonly YB_COMPILER_TYPE
  fi
}

is_clang() {
  if [[ $YB_COMPILER_TYPE == "clang" ]]; then
    return 0
  else
    return 1
  fi
}

is_gcc() {
  if [[ $YB_COMPILER_TYPE == "gcc" ]]; then
    return 0
  else
    return 1
  fi
}

is_ubuntu() {
  [[ -f /etc/issue ]] && grep -q Ubuntu /etc/issue
}

build_compiler_if_necessary() {
  # Sometimes we have to build the compiler before we can run CMake.
  if is_clang && is_linux; then
    log "Building clang before we can run CMake with compiler pointing to clang"
    "$YB_THIRDPARTY_DIR/build_thirdparty.sh" llvm
  fi
}

set_compiler_type_based_on_jenkins_job_name() {
  if [[ -n "${YB_COMPILER_TYPE:-}" ]]; then
    if [[ -n "${JOB_NAME:-}" ]]; then
      log "The YB_COMPILER_TYPE variable is already set to '${YB_COMPILER_TYPE}', not setting it" \
          "based on the Jenkins job name."
    fi
  else
    local compiler_type
    local jenkins_job_name=$( echo "$JOB_NAME" | to_lowercase )
    YB_COMPILER_TYPE=""
    for compiler_type in "${VALID_COMPILER_TYPES[@]}"; do
      if [[ "-$jenkins_job_name-" =~ [-_]$compiler_type[-_] ]]; then
        log "Setting YB_COMPILER_TYPE='$compiler_type' based on Jenkins job name '$JOB_NAME'."
        YB_COMPILER_TYPE=$compiler_type
        break
      fi
    done
    if [[ -z "$YB_COMPILER_TYPE" ]]; then
      log "Could not determine compiler type from Jenkins job name '$JOB_NAME'," \
          "will use the default."
      return
    fi
  fi
  validate_compiler_type
  readonly YB_COMPILER_TYPE
  export YB_COMPILER_TYPE
}

validate_compiler_type() {
  local compiler_type
  if [[ $# -eq 0 ]]; then
    if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
      fatal "$FUNCNAME is called with no arguments but YB_COMPILER_TYPE is not set or is empty"
    fi
    compiler_type=$YB_COMPILER_TYPE
  elif [[ $# -eq 1 ]]; then
    compiler_type=$1
  else
    fatal "$FUNCNAME can only be called with 0 or 1 argument, got $# arguments: $*"
  fi

  if [[ ! $compiler_type =~ $VALID_COMPILER_TYPES_RE ]]; then
    fatal "Invalid compiler type: YB_COMPILER_TYPE='$compiler_type'" \
          "(expected one of: ${VALID_COMPILER_TYPES[@]})."
  fi
}

validate_cmake_build_type() {
  expect_num_args 1 "$@"
  local _cmake_build_type=$1
  _cmake_build_type=$( echo "$_cmake_build_type" | tr A-Z a-z )
  if [[ ! "$_cmake_build_type" =~ $VALID_CMAKE_BUILD_TYPES_RE ]]; then
    fatal "Invalid CMake build type (what we're about to pass to our CMake build as" \
          "_cmake_build_type): '$_cmake_build_type'." \
          "Valid CMake build types are: ${VALID_CMAKE_BUILD_TYPES[@]}."
  fi
}

ensure_using_clang() {
  if [[ -n ${YB_COMPILER_TYPE:-} && $YB_COMPILER_TYPE != "clang" ]]; then
    fatal "ASAN/TSAN builds require clang," \
          "but YB_COMPILER_TYPE is already set to '$YB_COMPILER_TYPE'"
  fi
  YB_COMPILER_TYPE="clang"
}

enable_tsan() {
  cmake_opts+=( -DYB_USE_TSAN=1 )
  ensure_using_clang
}

# This performs two configuration actions:
# - Sets cmake_build_type based on build_type. cmake_build_type is what's being passed to CMake
#   using the CMAKE_BUILD_TYPE variable. CMAKE_BUILD_TYPE can't be "asan" or "tsan".
# - Ensure the YB_COMPILER_TYPE environment variable is set. It is used by our compiler-wrapper.sh
#   script to invoke the appropriate C/C++ compiler.
set_cmake_build_type_and_compiler_type() {
  if [[ -z "${cmake_opts:-}" ]]; then
    cmake_opts=()
  fi

  if [[ -z ${build_type:-} ]]; then
    log "Setting build type to 'debug' by default"
    build_type=debug
  fi

  normalize_build_type
  # We're relying on build_type to set more variables, so make sure it does not change later.
  readonly build_type

  case "$build_type" in
    asan)
      cmake_opts+=( -DYB_USE_ASAN=1 -DYB_USE_UBSAN=1 )
      cmake_build_type=fastdebug
      ensure_using_clang
    ;;
    tsan)
      enable_tsan
      cmake_build_type=fastdebug
    ;;
    tsan_slow)
      enable_tsan
      cmake_build_type=debug
    ;;
    idebug|irelease|ifastdebug)
      cmake_build_type=${build_type:1}
      cmake_opts+=( -DYB_INSTRUMENT_FUNCTIONS=1 )
    ;;
    compilecmds)
      cmake_build_type=debug
      export CMAKE_EXPORT_COMPILE_COMMANDS=1
      export YB_EXPORT_COMPILE_COMMANDS=1
    ;;
    *)
      cmake_build_type=$build_type
  esac
  validate_cmake_build_type "$cmake_build_type"
  readonly cmake_build_type

  if is_mac; then
    if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
      YB_COMPILER_TYPE=clang
    elif [[ $YB_COMPILER_TYPE != "clang" ]]; then
      fatal "YB_COMPILER_TYPE can only be 'clang' on Mac OS X," \
            "found YB_COMPILER_TYPE=$YB_COMPILER_TYPE."
    fi
  elif [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    # The default on Linux.
    YB_COMPILER_TYPE=gcc
  fi

  validate_compiler_type
  readonly YB_COMPILER_TYPE
  export YB_COMPILER_TYPE

  # We need to set CMAKE_C_COMPILER and CMAKE_CXX_COMPILER outside of CMake. We used to do that from
  # CMakeLists.txt, and got into an infinite loop where CMake kept saying:
  #
  #   You have changed variables that require your cache to be deleted.
  #   Configure will be re-run and you may have to reset some variables.
  #   The following variables have changed:
  #   CMAKE_CXX_COMPILER= /usr/bin/c++
  #
  # Not sure why it printed the old value there, since we tried to assign it the new value, the
  # same as what's given below.
  #
  # So our new approach is to pass the correct command-line options to CMake, and still let CMake
  # use the default compiler in CLion-triggered builds.

  cmake_opts+=( "-DCMAKE_BUILD_TYPE=$cmake_build_type" )
  cmake_opts+=( "${YB_DEFAULT_CMAKE_OPTS[@]}" )

  if using_ninja; then
    cmake_opts+=( -G Ninja )
    make_program=ninja
    if ! which ninja &>/dev/null; then
      if using_linuxbrew; then
        export YB_NINJA_PATH=$YB_LINUXBREW_DIR/bin/ninja
        make_program=$YB_NINJA_PATH
      elif using_custom_homebrew; then
        export YB_NINJA_PATH=$YB_CUSTOM_HOMEBREW_DIR/bin/ninja
        make_program=$YB_NINJA_PATH
      elif is_mac; then
        log "Did not find the 'ninja' executable, auto-installing ninja using Homebrew"
        brew install ninja
      fi
    fi
    make_file=build.ninja
  else
    make_program=make
    make_file=Makefile
  fi

  cmake_opts+=( -DCMAKE_MAKE_PROGRAM=$make_program )
}

set_mvn_parameters() {
  local should_use_shared_dirs=false
  should_copy_artifacts_to_non_shared_repo=false
  if is_jenkins && is_src_root_on_nfs; then
    if is_mac && "$is_run_test_script" && [[ -n ${YB_TMP_GROUP_ID:-} ]]; then
      should_use_shared_dirs=false
      should_copy_artifacts_to_non_shared_repo=true
      log "Will not use shared Maven repository ($YB_SHARED_MVN_LOCAL_REPO), but will copy" \
          "the artifact with group id ${YB_TMP_GROUP_ID:-undefined} from it to" \
          "$YB_NON_SHARED_MVN_LOCAL_REPO"
    else
      should_use_shared_dirs=true
      log "Will use shared Maven repository ($YB_SHARED_MVN_LOCAL_REPO)." \
          "Based on parameters: is_run_test_script=$is_run_test_script," \
          "YB_TMP_GROUP_ID=${YB_TMP_GROUP_ID:-undefined}," \
          "OSTYPE=$OSTYPE"
    fi
  fi
  if [[ -z ${YB_MVN_LOCAL_REPO:-} ]]; then
    if "$should_use_shared_dirs"; then
      YB_MVN_LOCAL_REPO=$YB_SHARED_MVN_LOCAL_REPO
    else
      YB_MVN_LOCAL_REPO=$YB_NON_SHARED_MVN_LOCAL_REPO
    fi
  fi
  export YB_MVN_LOCAL_REPO

  if [[ -z ${YB_MVN_SETTINGS_PATH:-} ]]; then
    if "$should_use_shared_dirs"; then
      YB_MVN_SETTINGS_PATH=$YB_SHARED_MVN_SETTINGS
    else
      YB_MVN_SETTINGS_PATH=$HOME/.m2/settings.xml
    fi
  fi
  export MVN_SETTINGS_PATH

  mvn_common_options=(
    --batch-mode
    -Dmaven.repo.local="$YB_MVN_LOCAL_REPO"
    -Dyb.thirdparty.dir="$YB_THIRDPARTY_DIR"
    -DbinDir="$BUILD_ROOT/bin"
  )
  log "The result of set_mvn_parameters:" \
      "YB_MVN_LOCAL_REPO=$YB_MVN_LOCAL_REPO," \
      "YB_MVN_SETTINGS_PATH=$YB_MVN_SETTINGS_PATH," \
      "should_copy_artifacts_to_non_shared_repo=$should_copy_artifacts_to_non_shared_repo"
}

# Put a retry loop here since it is possible that multiple concurrent builds will try to do this
# at the same time. However, this should converge quickly.
rsync_with_retries() {
  declare -i attempt=1
  declare -i -r max_attempts=5
  while true; do
    if ( set -x; rsync "$@" ); then
      return
    fi
    if [[ $attempt -eq $max_attempts ]]; then
      log "rsync failed after $max_attempts attempts, giving up"
      return 1
    fi
    log "This was rsync attempt $attempt out of $max_attempts. Re-trying after a delay."
    sleep 1
    let attempt+=1
  done
}

copy_artifacts_to_non_shared_mvn_repo() {
  if ! "$should_copy_artifacts_to_non_shared_repo"; then
    return
  fi
  local group_id_rel_path=${YB_TMP_GROUP_ID//./\/}
  local src_dir=$YB_SHARED_MVN_LOCAL_REPO/$group_id_rel_path
  local dest_dir=$YB_MVN_LOCAL_REPO/$group_id_rel_path
  log "Copying Maven artifacts from '$src_dir' to '$dest_dir'"
  mkdir -p "${dest_dir%/*}"
  rsync_with_retries -az "$src_dir/" "$dest_dir"
  log "Copying non-YB artifacts from '$YB_SHARED_MVN_LOCAL_REPO' to '$YB_MVN_LOCAL_REPO'"
  rsync_with_retries "$YB_SHARED_MVN_LOCAL_REPO/" "$YB_MVN_LOCAL_REPO" --exclude 'org/yb*'
}

# Appends the settings path specified by $YB_MVN_SETTINGS_PATH (in case that path exists), as well
# as other common Maven options used across all invocations of Maven, to the mvn_opts array. The
# caller of this function usually declares mvn_opts as a local array.
append_common_mvn_opts() {
  mvn_opts+=(
    "${mvn_common_options[@]}"
  )
  if [[ -f $YB_MVN_SETTINGS_PATH ]]; then
    mvn_opts+=(
      --settings "$YB_MVN_SETTINGS_PATH"
    )
  elif [[ $YB_MVN_SETTINGS_PATH != $HOME/.m2/settings.xml ]]; then
    log "Non-default maven user settings file specified by YB_MVN_SETTINGS_PATH does not exist:" \
        "'$YB_MVN_SETTINGS_PATH'"
  fi
}

# A utility function called by both 'build_yb_java_code' and 'build_yb_java_code_with_retries'.
build_yb_java_code_filter_save_output() {
  set_mvn_parameters
  log "Building Java code in $PWD"

  # --batch-mode hides download progress.
  # We are filtering out some patterns from Maven output, e.g.:
  # [INFO] META-INF/NOTICE already added, skipping
  # [INFO] Downloaded: https://repo.maven.apache.org/maven2/org/codehaus/plexus/plexus-classworlds/2.4/plexus-classworlds-2.4.jar (46 KB at 148.2 KB/sec)
  # [INFO] Downloading: https://repo.maven.apache.org/maven2/org/apache/maven/doxia/doxia-logging-api/1.1.2/doxia-logging-api-1.1.2.jar
  local has_local_output=false # default is output path variable is set by calling function
  if [[ -z ${java_build_output_path:-} ]]; then
    local java_build_output_path=/tmp/yb-java-build-$( get_timestamp ).$$.tmp
    has_local_output=true
  fi
  local mvn_opts=()
  append_common_mvn_opts
  if ! is_jenkins; then
    mvn_opts+=( -Dmaven.javadoc.skip )
  fi
  set +e -x  # +e: do not fail on grep failure, -x: print the command to stderr.
  if mvn "${mvn_opts[@]}" "$@" 2>&1 | \
      egrep -v --line-buffered "$MVN_OUTPUT_FILTER_REGEX" | \
      tee "$java_build_output_path"; then
    set +x # stop printing commands
    # We are testing for mvn build failure with grep, since we run mvn with '--fail-never' which
    # always returns success. '--fail-at-end' could have been another possibility, but that mode
    # skips dependent modules so most tests are often not run. Therefore, we resort to grep.
    egrep "BUILD SUCCESS" "$java_build_output_path" &>/dev/null
    local mvn_exit_code=$?
    set -e
    if [[ $has_local_output == "true" ]]; then
      rm -f "$java_build_output_path" # cleaning up
    fi
    if [[ $mvn_exit_code -eq 0 ]]; then
      log "Java build SUCCEEDED"
    else
      # Useful for searching in console output.
      log "Java build FAILED: could not find 'BUILD SUCCESS' in Maven output"
    fi
    return $mvn_exit_code
  fi
  set -e +x
  log "Java build or one of its output filters failed"
  if [[ -f $java_build_output_path ]]; then
    log "Java build output (from '$java_build_output_path'):"
    cat "$java_build_output_path"
    log "(End of Java build output)"
    rm -f "$java_build_output_path"
  else
    log "Java build output path file not found at '$java_build_output_path'"
  fi
  return 1
}

build_yb_java_code() {
  local java_build_output_path=/tmp/yb-java-build-$( get_timestamp ).$$.tmp
  build_yb_java_code_filter_save_output "$@"
  local mvn_exit_code=$?
  rm -f "$java_build_output_path"
  return $mvn_exit_code
}

build_yb_java_code_with_retries() {
  local java_build_output_path=/tmp/yb-java-build-$( get_timestamp ).$$.tmp
  declare -i attempt=1

  while [[ $attempt -le $MAX_JAVA_BUILD_ATTEMPTS ]]; do
    if build_yb_java_code_filter_save_output "$@"; then
      rm -f "$java_build_output_path"
      return 0
    fi

    if grep "Could not transfer artifact" "$java_build_output_path" >/dev/null; then
      log "Java build attempt $attempt failed due to temporary connectivity issues, re-trying."
    else
      return 1
    fi

    rm -f "$java_build_output_path"

    let attempt+=1
  done
  return 1
}

build_yb_java_code_in_all_dirs() {
  local java_project_dir
  for java_project_dir in "${yb_java_project_dirs[@]}"; do
    pushd "$java_project_dir"
    if ! time build_yb_java_code_with_retries "$@"; then
      log "Failed to build Java code in directory '$java_project_dir'" \
          "with these Maven arguments: $*"
      return 1
    fi
    popd
  done
}

# Create a directory on an ephemeral drive and link it into the given target location. If there are
# no ephemeral drives, create the directory in place.
# Parameters:
#   target_path - The target path to create the directory or symlink at.
#   directory_identifier - A unique identifier that will be used in naming the new directory
#                          created on an ephemeral drive.
create_dir_on_ephemeral_drive() {
  expect_num_args 2 "$@"
  local target_path=$1
  local directory_identifier=$2

  if [[ -z ${num_ephemeral_drives:-} ]]; then
    # Collect available ephemeral drives. This is only done once.
    local ephemeral_mountpoint
    # EPHEMERAL_DRIVES_FILTER_REGEX is not supposed to be anchored in the end, so we need to add
    # a "$" to filter ephemeral mountpoints correctly.
    ephemeral_drives=()
    for ephemeral_mountpoint in $EPHEMERAL_DRIVES_GLOB; do
      if [[ -d $ephemeral_mountpoint &&
            $ephemeral_mountpoint =~ $EPHEMERAL_DRIVES_FILTER_REGEX$ ]]; then
        ephemeral_drives+=( "$ephemeral_mountpoint" )
      fi
    done

    declare -r -i num_ephemeral_drives=${#ephemeral_drives[@]}  # "-r -i" means readonly integer.
  fi

  if [[ $num_ephemeral_drives -eq 0 ]]; then
    if [[ -n ${YB_VERBOSE:-} && ! -d $target_path ]]; then
      log "No ephemeral drives found, creating directory '$target_path' in place."
    fi
    mkdir_safe "$target_path"
  else
    local random_drive=${ephemeral_drives[$RANDOM % $num_ephemeral_drives]}
    local actual_dir=$random_drive/${USER}__$jenkins_job_and_build/$directory_identifier
    mkdir_safe "$actual_dir"

    # Create the parent directory that we'll be creating a link in, if necessary.
    if [[ ! -d ${target_path%/*} ]]; then
      log "Directory $target_path does not exist, creating it before creating a symlink inside."
      mkdir_safe "${target_path%/*}"
    fi

    ln -s "$actual_dir" "$target_path"
    log "Created '$target_path' as a symlink to an ephemeral drive location '$actual_dir'."
  fi
}

# Skip the most part of the normal C++ build output. Still keep the "100%" lines so we can see
# if the build runs to completion. This only filters stdin, so it is expected that stderr is
# redirected to stdout when invoking the C++ build.
filter_boring_cpp_build_output() {
  egrep -v --line-buffered "\
^(\[ *[0-9]{1,2}%\] +)*(\
Building C(XX)? object |\
Running C[+][+] protocol buffer compiler (with YRPC plugin )?on |\
Linking CXX ((static|shared )?library|executable) |\
Built target \
)|\
Scanning dependencies of target |\
^ssh: connect to host .* port [0-9]+: Connection (timed out|refused)|\
Host .* seems to be down, retrying on a different host|\
Connection to .* closed by remote host.|\
ssh: Could not resolve hostname build-workers-.*: Name or service not known"
}


put_path_entry_first() {
  expect_num_args 1 "$@"
  local path_entry=$1
  remove_path_entry "$path_entry"
  export PATH=$path_entry:$PATH
}

add_path_entry() {
  expect_num_args 1 "$@"
  local path_entry=$1
  if [[ $PATH != *:$path_entry && $PATH != $path_entry:* && $PATH != *:$path_entry:* ]]; then
    export PATH+=:$path_entry
  fi
}

# Removes the ccache wrapper directory from PATH so we can find the real path to a compiler, e.g.
# /usr/bin/gcc instead of /usr/lib64/ccache/gcc.  This is expected to run in a subshell so that we
# don't make any unexpected changes to the script's PATH.
# TODO: how to do this properly on Mac OS X?
remove_ccache_dir_from_path() {
  remove_path_entry /usr/lib64/ccache
}

# Given a compiler type, e.g. gcc or clang, find the actual compiler executable (not a wrapper
# provided by ccache).  Takes into account YB_GCC_PREFIX and YB_CLANG_PREFIX variables that allow to
# use custom gcc and clang installations. Sets cc_executable and cxx_executable variables. This is
# used in compiler-wrapper.sh.
find_compiler_by_type() {
  compiler_type=$1
  validate_compiler_type "$1"
  local compiler_type=$1
  unset cc_executable
  unset cxx_executable
  case "$compiler_type" in
    gcc)
      if [[ -n ${YB_GCC_PREFIX:-} ]]; then
        if [[ ! -d $YB_GCC_PREFIX/bin ]]; then
          fatal "Directory YB_GCC_PREFIX/bin ($YB_GCC_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_GCC_PREFIX/bin/gcc
        cxx_executable=$YB_GCC_PREFIX/bin/g++
      elif using_linuxbrew; then
        cc_executable=$YB_LINUXBREW_DIR/bin/gcc
        cxx_executable=$YB_LINUXBREW_DIR/bin/g++
      else
        cc_executable=gcc
        cxx_executable=g++
      fi
    ;;
    gcc8)
      if [[ -n ${YB_GCC8_PREFIX:-} ]]; then
        if [[ ! -d $YB_GCC8_PREFIX/bin ]]; then
          fatal "Directory YB_GCC_PREFIX/bin ($YB_GCC_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_GCC8_PREFIX/bin/gcc-8
        cxx_executable=$YB_GCC8_PREFIX/bin/g++-8
      else
        cc_executable=$(which gcc-8)
        cxx_executable=$(which g++-8)
      fi
    ;;
    clang)
      if [[ -n ${YB_CLANG_PREFIX:-} ]]; then
        if [[ ! -d $YB_CLANG_PREFIX/bin ]]; then
          fatal "Directory \$YB_CLANG_PREFIX/bin ($YB_CLANG_PREFIX/bin) does not exist"
        fi
        cc_executable=$YB_CLANG_PREFIX/bin/clang
      elif [[ $OSTYPE =~ ^darwin ]]; then
        cc_executable=/usr/bin/clang
      else
        local clang_path
        local clang_found=false
        local clang_paths_to_try=(
          "$YB_THIRDPARTY_DIR/clang-toolchain/bin/clang"
          # clang is present in this location in pre-built third-party archives built before
          # the transition to Linuxbrew (https://phabricator.dev.yugabyte.com/D982). This can be
          # removed when the transition is complete.
          "$YB_THIRDPARTY_DIR/installed/common/bin/clang"
        )
        for clang_path in "${clang_paths_to_try[@]}"; do
          if [[ -f $clang_path ]]; then
            cc_executable=$clang_path
            clang_found=true
            break
          fi
        done
        if ! "$clang_found"; then
          fatal "Failed to find clang at the following locations: ${clang_paths_to_try[@]}"
        fi
      fi
      if [[ -z ${cxx_executable:-} ]]; then
        cxx_executable=$cc_executable++  # clang -> clang++
      fi
    ;;
    zapcc)
      if [[ -n ${YB_ZAPCC_INSTALL_PATH:-} ]]; then
        cc_executable=$YB_ZAPCC_INSTALL_PATH/bin/zapcc
        cxx_executable=$YB_ZAPCC_INSTALL_PATH/bin/zapcc++
      else
        cc_executable=zapcc
        cxx_executable=zapcc++
      fi
    ;;
    *)
      fatal "Unknown compiler type '$compiler_type'"
  esac

  # -----------------------------------------------------------------------------------------------
  # Validate existence of compiler executables.
  # -----------------------------------------------------------------------------------------------

  local compiler_var_name
  for compiler_var_name in cc_executable cxx_executable; do
    if [[ -n ${!compiler_var_name:-} ]]; then
      local compiler_path=${!compiler_var_name}
      if [[ ! -x $compiler_path && $compiler_path =~ ^[a-z+]+$ ]]; then
        # This is a plain "gcc/g++/clang/clang++" compiler command name. Try to find the exact
        # compiler path using the "which" command.
        set +e
        compiler_path=$( remove_ccache_dir_from_path && which "${!compiler_var_name}" )
        if [[ $? -ne 0 ]]; then
          # "which" did not work, revert to the old value.
          compiler_path=${!compiler_var_name}
        fi
        set -e
      fi

      if [[ ! -x $compiler_path ]]; then
        fatal "Compiler executable does not exist at the path we set $compiler_var_name to" \
              "(possibly applying 'which' expansion): $compiler_path" \
              "(trying to use compiler type '$compiler_type')."
      fi
      eval $compiler_var_name=\"$compiler_path\"
    fi
  done
}

# Make pushd and popd quiet.
# http://stackoverflow.com/questions/25288194/dont-display-pushd-popd-stack-accross-several-bash-scripts-quiet-pushd-popd
pushd() {
  local dir_name=$1
  if [[ ! -d $dir_name ]]; then
    fatal "Directory '$dir_name' does not exist"
  fi
  command pushd "$@" > /dev/null
}

popd() {
  command popd "$@" > /dev/null
}

detect_brew() {
  if is_linux; then
    detect_linuxbrew
  elif is_mac; then
    detect_custom_homebrew
  else
    log "Not a Linux or a macOS platform, the detect_brew function is a no-op."
  fi
}

download_and_extract_archive() {
  expect_num_args 2 "$@"
  extracted_dir=""

  local url=$1
  local dest_dir_parent=$2
  local tar_gz_name=${url##*/}
  local install_dir_name=${tar_gz_name%.tar.gz}
  local dest_dir=$dest_dir_parent/$install_dir_name
  if [[ ! -d $dest_dir && ! -L $dest_dir ]]; then
    if [[ ! -d $YB_DOWNLOAD_LOCKS_DIR ]]; then
      ( umask 0; mkdir -p "$YB_DOWNLOAD_LOCKS_DIR" )
    fi
    (
      umask 0
      lock_path=$YB_DOWNLOAD_LOCKS_DIR/$install_dir_name
      (
        flock -w "$YB_DOWNLOAD_LOCK_TIMEOUT_SEC" 200
        if [[ ! -d $dest_dir && ! -L $dest_dir ]]; then
          log "[Host $(hostname)] Acquired lock $lock_path, proceeding with archive installation."
          (
            set -x
            "$YB_SRC_ROOT/python/yb/download_and_extract_archive.py" \
              --url "$url" --dest-dir-parent "$dest_dir_parent"
          )
        else
          log "[Host $(hostname)] Acquired lock $lock_path but directory $dest_dir already" \
              "exists. This is OK."
        fi
      ) 200>"$lock_path"
    )
  fi
  extracted_dir=$dest_dir
}

download_thirdparty() {
  download_and_extract_archive "$YB_THIRDPARTY_URL" /opt/yb-build/thirdparty
  export YB_THIRDPARTY_DIR=$extracted_dir
  local linuxbrew_url_path=$YB_THIRDPARTY_DIR/linuxbrew_url.txt
  if [[ -f $linuxbrew_url_path ]]; then
    local linuxbrew_url=$(<"$linuxbrew_url_path")
    download_and_extract_archive "$linuxbrew_url" /opt/yb-build/brew
    export YB_LINUXBREW_DIR=$extracted_dir
  else
    fatal "Cannot download Linuxbrew: file $linuxbrew_url_path does not exist"
  fi
}

install_linuxbrew() {
  if ! is_linux; then
    fatal "Expected this function to only be called on Linux"
  fi
  if is_ubuntu; then
    return
  fi
  local version=$1
  local linuxbrew_dirname=linuxbrew-$version
  local linuxbrew_dir=$YB_LINUXBREW_LOCAL_ROOT/$linuxbrew_dirname
  local linuxbrew_archive="${linuxbrew_dir}.tar.gz"
  local linuxbrew_archive_checksum="${linuxbrew_archive}.sha256"
  local url="https://github.com/YugaByte/brew-build/releases/download/$version/\
linuxbrew-$version.tar.gz"
  mkdir -p "$YB_LINUXBREW_LOCAL_ROOT"
  if [[ ! -f $linuxbrew_archive ]]; then
    echo "Downloading Linuxbrew from $url..."
    rm -f "$linuxbrew_archive_checksum"
    curl -L "$url" -o "$linuxbrew_archive"
  fi
  if [[ ! -f $linuxbrew_archive_checksum ]]; then
    echo "Downloading Linuxbrew archive checksum file for $url..."
    curl -L "$url.sha256" -o "$linuxbrew_archive_checksum"
  fi
  echo "Verifying Linuxbrew archive checksum ..."
  pushd "$YB_LINUXBREW_LOCAL_ROOT"
  sha256sum -c --strict "$linuxbrew_archive_checksum"
  popd
  echo "Installing Linuxbrew into $linuxbrew_dir..."
  local tmp=$YB_LINUXBREW_LOCAL_ROOT/tmp/$$_$RANDOM$RANDOM
  mkdir -p "$tmp"
  tar zxf "$linuxbrew_archive" -C "$tmp"
  if mv "$tmp/$linuxbrew_dirname" "$YB_LINUXBREW_LOCAL_ROOT/"; then
    pushd "$linuxbrew_dir"
    ./post_install.sh
    popd
  fi
}

try_set_linuxbrew_dir() {
  local linuxbrew_dir=$1
  if [[ -d "$linuxbrew_dir" &&
        -d "$linuxbrew_dir/bin" &&
        -d "$linuxbrew_dir/lib" &&
        -d "$linuxbrew_dir/include" ]]; then
    export YB_LINUXBREW_DIR=$(realpath "$linuxbrew_dir")
    return 0
  else
    return 1
  fi
}

# -------------------------------------------------------------------------------------------------
# Detecting Homebrew/Linuxbrew
# -------------------------------------------------------------------------------------------------

wait_for_directory_existence() {
  expect_num_args 1 "$@"
  local dir_path=$1
  declare -i attempt=0
  while [[ ! -d $dir_path ]]; do
    if [[ $attempt -ge $DIRECTORY_EXISTENCE_WAIT_TIMEOUT_SEC ]]; then
      fatal "Gave up waiting for directory '$dir_path' to appear after $attempt seconds"
    fi
    log "Directory '$dir_path' not found, waiting for it to mount"
    ( set +e; ls "$dir_path"/* >/dev/null )
    let attempt+=1
    sleep 1
  done
}

detect_linuxbrew() {
  if ! is_linux; then
    fatal "Expected this function to only be called on Linux"
  fi
  if [[ -n ${YB_LINUXBREW_DIR:-} ]]; then
    export YB_LINUXBREW_DIR
    return
  fi

  unset YB_LINUXBREW_DIR
  if ! is_linux; then
    return
  fi
  if is_ubuntu; then
    # Not using Linuxbrew on Ubuntu.
    return
  fi

  local version_file=$YB_SRC_ROOT/thirdparty/linuxbrew_version.txt
  if [[ ! -f $version_file ]]; then
    fatal "'$version_file' does not exist"
  fi
  local linuxbrew_version=$( read_file_and_trim "$version_file" )
  local linuxbrew_dirname="linuxbrew-$linuxbrew_version"

  local candidates=()
  local jenkins_linuxbrew_dir="$SHARED_LINUXBREW_BUILDS_DIR/$linuxbrew_dirname"
  if [[ -d $jenkins_linuxbrew_dir ]]; then
    candidates=( "$jenkins_linuxbrew_dir" )
  elif is_jenkins; then
    if is_src_root_on_nfs; then
      wait_for_directory_existence "$jenkins_linuxbrew_dir"
      candidates=( "$jenkins_linuxbrew_dir" )
    else
      yb_fatal_exit_code=$YB_EXIT_CODE_NO_SUCH_FILE_OR_DIRECTORY
      fatal "Warning: Linuxbrew directory referenced by '$version_file' does not" \
            "exist: '$jenkins_linuxbrew_dir', refusing to proceed to prevent " \
            "non-deterministic builds."
    fi
  fi

  if [[ ${#candidates[@]} -gt 0 ]]; then
    local linuxbrew_dir
    for linuxbrew_dir in "${candidates[@]}"; do
      if try_set_linuxbrew_dir "$linuxbrew_dir"; then
        return
      fi
    done
  fi

  local linuxbrew_local_dir="$YB_LINUXBREW_LOCAL_ROOT/$linuxbrew_dirname"
  if ! is_jenkins && [[ ! -d $linuxbrew_local_dir ]]; then
    install_linuxbrew "$linuxbrew_version"
  fi
  if ! try_set_linuxbrew_dir "$linuxbrew_local_dir"; then
    if [[ ${#candidates[@]} -gt 0 ]]; then
      log "Could not find Linuxbrew in any of these directories: ${candidates[@]}."
    else
      log "Could not find Linuxbrew candidate directories."
    fi
    log "Failed to install Linuxbrew $linuxbrew_version into $linuxbrew_local_dir."
  fi
}

# -------------------------------------------------------------------------------------------------
# Similar to detect_linuxbrew, but for macOS.
# This function was created by copying detect_linuxbrew and replacing Linuxbrew with Homebrew
# in a few places. This was done to avoid destabilizing the Linux environment. Rather than
# refactoring detect_custom_homebrew and detect_linuxbrew functions to extract common parts, we will
# leave that until our whole build environment framework is rewritten in Python.
# Mikhail Bautin, 11/14/2018
# -------------------------------------------------------------------------------------------------
detect_custom_homebrew() {
  if ! is_mac; then
    fatal "Expected this function to only be called on macOS"
  fi
  if [[ -n ${YB_CUSTOM_HOMEBREW_DIR:-} ]]; then
    export YB_CUSTOM_HOMEBREW_DIR
    return
  fi
  local candidates=(
    "$HOME/.homebrew-yb-build"
  )

  local version_for_jenkins_file=$YB_SRC_ROOT/thirdparty/homebrew_version_for_jenkins.txt
  if [[ -f $version_for_jenkins_file ]]; then
    local version_for_jenkins=$( read_file_and_trim "$version_for_jenkins_file" )
    preferred_homebrew_dir="$SHARED_CUSTOM_HOMEBREW_BUILDS_DIR/homebrew_$version_for_jenkins"
    if [[ -d $preferred_homebrew_dir ]]; then
      if is_jenkins_user; then
        # If we're running on Jenkins (or building something for consumption by Jenkins under the
        # "jenkins" user), then the "Homebrew for Jenkins" directory takes precedence.
        candidates=( "$preferred_homebrew_dir" "${candidates[@]}" )
      else
        # Otherwise, the user's local Homebrew build takes precedence.
        candidates=( "${candidates[@]}" "$preferred_homebrew_dir" )
      fi
    elif is_jenkins; then
      if is_src_root_on_nfs; then
        wait_for_directory_existence "$preferred_homebrew_dir"
      else
        yb_fatal_exit_code=$YB_EXIT_CODE_NO_SUCH_FILE_OR_DIRECTORY
        fatal "Warning: Homebrew directory referenced by '$version_for_jenkins_file' does not" \
              "exist: '$preferred_homebrew_dir', refusing to proceed to prevent " \
              "non-deterministic builds."
      fi
    fi
  elif is_jenkins; then
    log "Warning: '$version_for_jenkins_file' does not exist"
  fi

  local homebrew_dir
  for homebrew_dir in "${candidates[@]}"; do
    if [[ -d "$homebrew_dir" &&
          -d "$homebrew_dir/bin" &&
          -d "$homebrew_dir/lib" &&
          -d "$homebrew_dir/include" ]]; then
      export YB_CUSTOM_HOMEBREW_DIR=$homebrew_dir
      break
    fi
  done
}
# -------------------------------------------------------------------------------------------------
# End of the detect_custom_homebrew that was created with by copying/pasting and editing
# the detect_linuxbrew function.
# -------------------------------------------------------------------------------------------------

using_linuxbrew() {
  if is_linux && [[ -n ${YB_LINUXBREW_DIR:-} ]]; then
    return 0  # true in bash
  fi
  return 1
}

using_custom_homebrew() {
  if is_mac && [[ -n ${YB_CUSTOM_HOMEBREW_DIR:-} ]]; then
    return 0  # true in bash
  fi
  return 1  # false in bash
}

set_use_ninja() {
  if [[ -z ${YB_USE_NINJA:-} ]]; then
    if [[ -n ${BUILD_ROOT:-} ]]; then
      if [[ $BUILD_ROOT == *-ninja ]]; then
        export YB_USE_NINJA=1
      else
        export YB_USE_NINJA=0
      fi
    else
      if which ninja &>/dev/null; then
        export YB_USE_NINJA=1
      elif using_linuxbrew; then
        local yb_ninja_path=$YB_LINUXBREW_DIR/bin/ninja
        if [[ -f $yb_ninja_path ]]; then
          export YB_USE_NINJA=1
          export YB_NINJA_PATH=$yb_ninja_path
        fi
      fi
    fi

    if using_ninja && [[ -z ${yb_ninja_path:-} ]]; then
      set +e
      local which_ninja=$( which ninja 2>/dev/null )
      set -e
      if using_linuxbrew; then
        local yb_ninja_path=$YB_LINUXBREW_DIR/bin/ninja
        if [[ ! -f $yb_ninja_path ]]; then
          fatal "When using Linuxbrew, Ninja must be installed as part of Linuxbrew, but this" \
                "file does not exist: $yb_ninja_path"
        fi
      elif using_custom_homebrew; then
        local yb_ninja_path=$YB_CUSTOM_HOMEBREW_DIR/bin/ninja
        if [[ ! -f $yb_ninja_path ]]; then
          fatal "When using custom Homebrew, Ninja must be installed as part of Linuxbrew, but" \
                "this file does not exist: $yb_ninja_path"
        fi
      elif [[ -f $which_ninja ]]; then
        local yb_ninja_path=$which_ninja
      else
        fatal "Could not set yb_ninja_path, ninja not found on PATH: $PATH"
      fi
    fi
  fi
}

using_ninja() {
  if [[ ${YB_USE_NINJA:-} == "1" ]]; then
    return 0
  else
    return 1
  fi
}

add_brew_bin_to_path() {
  if using_linuxbrew; then
    # We need to add Linuxbrew's bin directory to PATH so that we can find the right compiler and
    # linker.
    put_path_entry_first "$YB_LINUXBREW_DIR/bin"
  fi
  if using_custom_homebrew; then
    # The same for a custom Homebrew installation on macOS.
    put_path_entry_first "$YB_CUSTOM_HOMEBREW_DIR/bin"
  fi
}

detect_num_cpus() {
  if [[ ! ${YB_NUM_CPUS:-} =~ ^[0-9]+$ ]]; then
    if is_linux; then
      YB_NUM_CPUS=$(grep -c processor /proc/cpuinfo)
    elif is_mac; then
      YB_NUM_CPUS=$(sysctl -n hw.ncpu)
    else
      fatal "Don't know how to detect the number of CPUs on OS $OSTYPE."
    fi

    if [[ ! $YB_NUM_CPUS =~ ^[0-9]+$ ]]; then
      fatal "Invalid number of CPUs detected: '$YB_NUM_CPUS' (expected a number)."
    fi
  fi
}

detect_num_cpus_and_set_make_parallelism() {
  detect_num_cpus
  if [[ -z ${YB_MAKE_PARALLELISM:-} ]]; then
    if [[ ${YB_REMOTE_COMPILATION:-} == "1" ]]; then
      declare -i num_build_workers=$( wc -l "$YB_BUILD_WORKERS_FILE" | awk '{print $1}' )
      # Add one to the number of workers so that we cause the auto-scaling group to scale up a bit
      # by stressing the CPU on each worker a bit more.
      declare -i effective_num_build_workers=$(( $num_build_workers + 1 ))

      # However, make sure this number is within a reasonable range.
      if [[ $effective_num_build_workers -lt $MIN_EFFECTIVE_NUM_BUILD_WORKERS ]]; then
        effective_num_build_workers=$MIN_EFFECTIVE_NUM_BUILD_WORKERS
      fi
      if [[ $effective_num_build_workers -gt $MAX_EFFECTIVE_NUM_BUILD_WORKERS ]]; then
        effective_num_build_workers=$MAX_EFFECTIVE_NUM_BUILD_WORKERS
      fi

      YB_MAKE_PARALLELISM=$(( $effective_num_build_workers * $YB_NUM_CORES_PER_BUILD_WORKER ))
    else
      YB_MAKE_PARALLELISM=$YB_NUM_CPUS
    fi
  fi
  export YB_MAKE_PARALLELISM
}

validate_thirdparty_dir() {
  ensure_directory_exists "$YB_THIRDPARTY_DIR/build_definitions"
  ensure_directory_exists "$YB_THIRDPARTY_DIR/patches"
  ensure_file_exists "$YB_THIRDPARTY_DIR/build_definitions/__init__.py"
}

# Detect if we're running on Google Compute Platform. We perform this check lazily as there might be
# a bit of a delay resolving the domain name.
detect_gcp() {
  # How to detect if we're running on Google Compute Engine:
  # https://cloud.google.com/compute/docs/instances/managing-instances#dmi
  if [[ -n ${YB_PRETEND_WE_ARE_ON_GCP:-} ]] || \
     curl metadata.google.internal --silent --output /dev/null --connect-timeout 1; then
    readonly is_running_on_gcp_exit_code=0  # "true" exit code
  else
    readonly is_running_on_gcp_exit_code=1  # "false" exit code
  fi
}

is_running_on_gcp() {
  if [[ -z ${is_running_on_gcp_exit_code:-} ]]; then
    detect_gcp
  fi
  return "$is_running_on_gcp_exit_code"
}

is_jenkins_user() {
  [[ $USER == "jenkins" ]]
}

is_jenkins() {
  if [[ -n ${JOB_NAME:-} ]] && is_jenkins_user; then
    return 0  # Yes, we're running on Jenkins.
  fi
  return 1  # Probably running locally.
}

should_gzip_test_logs() {
  is_jenkins || [[ ${YB_GZIP_TEST_LOGS:-0} == "1" ]]
}

# For each file provided as an argument, gzip the given file if it exists and is not already
# compressed.
gzip_if_exists() {
  local f
  for f in "$@"; do
    if [[ -f $f && $f != *.gz && $f != *.bz2 ]]; then
      gzip "$f"
    fi
  done
}

# Check if we're in a Jenkins master build.
is_jenkins_master_build() {
  if [[ -n ${JOB_NAME:-} && $JOB_NAME = *-master-* ]]; then
    return 0
  fi
  return 1
}

# Check if we're in a Jenkins Phabricator build (a pre-commit build).
is_jenkins_phabricator_build() {
  if [[ -z ${JOB_NAME:-} ]]; then
    return 1  # No, not running on Jenkins.
  fi

  if [[ $JOB_NAME == *-phabricator-* || $JOB_NAME == *-phabricator ]]; then
    return 0  # Yes, this is a Phabricator build.
  fi

  return 1  # No, some other kind of Jenkins job.
}

# Check if we're using an NFS partition in YugaByte's build environment.
is_src_root_on_nfs() {
  if [[ $YB_SRC_ROOT =~ $YB_NFS_PATH_RE ]]; then
    return 0
  fi
  return 1
}

using_remote_compilation() {
  if [[ ! $YB_REMOTE_COMPILATION =~ ^(0|1)$ ]]; then
    # TODO: this will still return from the function as if the value is false. This is how bash
    # return values work.
    fatal "YB_REMOTE_COMPILATION is supposed to be 0 or 1 by the time using_remote_compilation is" \
          "called."
  fi
  if [[ ${YB_REMOTE_COMPILATION:-} == "1" ]]; then
    return 0  # "true" return value
  fi
  return 1  # "false" return value
}

# This is used for escaping command lines for remote execution.
# From StackOverflow: https://goo.gl/sTKReB
# Using this approach: "Put the whole string in single quotes. This works for all chars except
# single quote itself. To escape the single quote, close the quoting before it, insert the single
# quote, and re-open the quoting."
#
escape_cmd_line() {
  escape_cmd_line_rv=""
  for arg in "$@"; do
    escape_cmd_line_rv+=" '"${arg/\'/\'\\\'\'}"'"
    # This should be equivalent to the sed command below.  The quadruple backslash encodes one
    # backslash in the replacement string. We don't need that in the pure-bash implementation above.
    # sed -e "s/'/'\\\\''/g; 1s/^/'/; \$s/\$/'/"
  done
  # Remove the leading space if necessary.
  escape_cmd_line_rv=${escape_cmd_line_rv# }
}

debugging_remote_compilation() {
  [[ ${YB_DEBUG_REMOTE_COMPILATION:-undefined} == "1" ]]
}

cmd_line_to_env_vars_for_remote_cmd() {
  declare -i i=1
  YB_ENCODED_REMOTE_CMD_LINE=""
  # This must match the separator in remote_cmd.sh.
  declare -r ARG_SEPARATOR=$'=:\t:='
  for arg in "$@"; do
    # The separator used here must match the separator used in remote_cmd.sh.
    YB_ENCODED_REMOTE_CMD_LINE+=$arg$ARG_SEPARATOR
  done
  # This variable must be accessible to remote_cmd.sh on the other side of ssh.
  export YB_ENCODED_REMOTE_CMD_LINE
}

run_remote_cmd() {
  local build_host=$1
  local executable=$2
  shift 2
  cmd_line_to_env_vars_for_remote_cmd "$@"
  local ssh_args=(
    "$build_host"
    "$YB_BUILD_SUPPORT_DIR/remote_cmd.sh"
    "$PWD"
    "$PATH"
    "$executable"
  )
  if debugging_remote_compilation; then
    ( set -x; ssh "${ssh_args[@]}" ) 2>&1
  else
    ssh "${ssh_args[@]}"
  fi
}

configure_remote_compilation() {
  if [[ ! ${YB_REMOTE_COMPILATION:-auto} =~ ^(0|1|auto)$ ]]; then
    fatal "Invalid value of the YB_REMOTE_COMPILATION environment variable: can be '0', '1', or" \
          "'auto'. Actual value: ${YB_REMOTE_COMPILATION:-undefined}."
  fi
  # Automatically set YB_REMOTE_COMPILATION in an NFS GCP environment.
  if [[ ${YB_REMOTE_COMPILATION:-auto} == "auto" ]]; then
    if is_running_on_gcp && is_src_root_on_nfs; then
      log "Automatically enabling remote compilation (running in an NFS GCP environment). " \
          "Use YB_REMOTE_COMPILATION=0 (or the --no-remote ybd option) to disable this behavior."
      YB_REMOTE_COMPILATION=1
    else
      YB_REMOTE_COMPILATION=0
      if is_jenkins; then
        # Make it easier to diagnose why we're not using the distributed build. Only enable this on
        # Jenkins to avoid confusing output during development.
        log "Not using remote compilation: " \
            "YB_REMOTE_COMPILATION=${YB_REMOTE_COMPILATION:-undefined}. " \
            "See additional diagnostics below."
        is_running_on_gcp && log "Running on GCP." || log "This is not GCP."
        if is_src_root_on_nfs; then
          log "YB_SRC_ROOT ($YB_SRC_ROOT) appears to be on NFS in YugaByte's distributed" \
              "build setup."
        fi
      fi
    fi
  fi
  export YB_REMOTE_COMPILATION
}

read_file_and_trim() {
  expect_num_args 1 "$@"
  local file_name=$1
  if [[ -f $file_name ]]; then
    cat "$file_name" | sed -e 's/^[[:space:]]*//; s/[[:space:]]*$//'
  else
    log "File '$file_name' does not exist"
    return 1
  fi
}

using_default_thirdparty_dir() {
  if [[ -n ${YB_THIRDPARTY_DIR:-} &&
        $YB_THIRDPARTY_DIR != "$YB_SRC_ROOT/thirdparty" ]]; then
    # YB_THIRDPARTY_DIR is specified and is not the default location
    return 1
  fi
  return 0
}

find_thirdparty_by_url() {
  if [[ -n ${YB_THIRDPARTY_URL:-} ]]; then
    download_thirdparty
    export NO_REBUILD_THIRDPARTY=1
  fi
}

# In our internal environment we build third-party dependencies in separate directories on NFS
# so that we can use them across many builds.
find_thirdparty_dir() {
  if [[ -n ${YB_THIRDPARTY_URL:-} ]]; then
    find_thirdparty_by_url
    log "YB_THIRDPARTY_DIR=$YB_THIRDPARTY_DIR"
    log "YB_LINUXBREW_DIR=$YB_LINUXBREW_DIR"
    found_shared_thirdparty_dir=true
    return
  fi
  found_shared_thirdparty_dir=false
  local parent_dir_for_shared_thirdparty=$NFS_PARENT_DIR_FOR_SHARED_THIRDPARTY
  if [[ ! -d $parent_dir_for_shared_thirdparty ]]; then
    log "Parent directory for shared third-party directories" \
        "('$NFS_PARENT_DIR_FOR_SHARED_THIRDPARTY') does not exist, cannot use pre-built" \
        "third-party directory from there."
    return
  fi

  local version=$(
    read_file_and_trim \
      "$YB_BUILD_SUPPORT_DIR/shared_thirdparty_version_for_jenkins_${short_os_name}.txt"
  )
  local thirdparty_dir_suffix="yugabyte-thirdparty-${version}/thirdparty"
  local existing_thirdparty_dir="${parent_dir_for_shared_thirdparty}/${thirdparty_dir_suffix}"
  if [[ -d $existing_thirdparty_dir ]]; then
    log "Using existing third-party dependencies from $existing_thirdparty_dir"
    if is_jenkins; then
      log "Cleaning the old dedicated third-party dependency build in '$YB_SRC_ROOT/thirdparty'"
      unset YB_THIRDPARTY_DIR
      if ! ( set -x; "$YB_SRC_ROOT/thirdparty/clean_thirdparty.sh" --all ); then
        log "Failed to clean the old third-party directory. Ignoring this error."
      fi
    fi
    export YB_THIRDPARTY_DIR=$existing_thirdparty_dir
    found_shared_thirdparty_dir=true
    export NO_REBUILD_THIRDPARTY=1
    return
  fi

  log "Even though the top-level directory '$parent_dir_for_shared_thirdparty'" \
      "exists, we could not find a prebuilt shared third-party directory there that exists. " \
      "Falling back to building our own third-party dependencies."
}

handle_predefined_build_root_quietly=false

handle_predefined_build_root() {
  expect_num_args 0 "$@"
  if [[ -z ${predefined_build_root:-} ]]; then
    return
  fi

  if [[ -L $predefined_build_root ]]; then
    predefined_build_root=$( readlink "$predefined_build_root" )
  fi

  if [[ -d $predefined_build_root ]]; then
    predefined_build_root=$( cd "$predefined_build_root" && pwd )
  fi

  if [[ $predefined_build_root != $YB_BUILD_INTERNAL_PARENT_DIR/* && \
        $predefined_build_root != $YB_BUILD_EXTERNAL_PARENT_DIR/* ]]; then
    # Sometimes $predefined_build_root contains symlinks on its path.
    $YB_SRC_ROOT/build-support/validate_build_root.py \
      "$predefined_build_root" \
      "$YB_BUILD_INTERNAL_PARENT_DIR" \
      "$YB_BUILD_EXTERNAL_PARENT_DIR"
  fi

  local basename=${predefined_build_root##*/}

  if [[ $basename =~ $BUILD_ROOT_BASENAME_RE ]]; then
    local _build_type=${BASH_REMATCH[1]}
    local _compiler_type=${BASH_REMATCH[2]}
    local _linking_type=${BASH_REMATCH[3]}
    local _dash_ninja=${BASH_REMATCH[4]}
  else
    fatal "Could not parse build root directory name '$basename'" \
          "(full path: '$predefined_build_root'). Expected to match '$BUILD_ROOT_BASENAME_RE'."
  fi

  if [[ -z ${build_type:-} ]]; then
    build_type=$_build_type
    if ! "$handle_predefined_build_root_quietly"; then
      log "Setting build type to '$build_type' based on predefined build root ('$basename')"
    fi
    validate_build_type "$build_type"
  elif [[ $build_type != $_build_type ]]; then
    fatal "Build type from the build root ('$_build_type' from '$predefined_build_root') does " \
          "not match current build type ('$build_type')."
  fi

  if [[ -z ${YB_COMPILER_TYPE:-} ]]; then
    export YB_COMPILER_TYPE=$_compiler_type
    if ! "$handle_predefined_build_root_quietly"; then
      log "Automatically setting compiler type to '$YB_COMPILER_TYPE' based on predefined build" \
          "root ('$basename')"
    fi
  elif [[ $YB_COMPILER_TYPE != $_compiler_type ]]; then
    fatal "Compiler type from the build root ('$_compiler_type' from '$predefined_build_root') " \
          "does not match YB_COMPILER_TYPE ('$YB_COMPILER_TYPE')."
  fi

  export YB_USE_NINJA=${YB_USE_NINJA:-}
  if [[ $_dash_ninja == "-ninja" && -z ${YB_USE_NINJA:-} ]]; then
    if ! "$handle_predefined_build_root_quietly"; then
      log "Setting YB_USE_NINJA to 1 based on predefined build root ('$basename')"
    fi
    export YB_USE_NINJA=1
  elif [[ $_dash_ninja == "-ninja" && $YB_USE_NINJA != "1" || \
          $_dash_ninja != "-ninja" && $YB_USE_NINJA == "1" ]]; then
    fatal "The use of ninja from build root ('$predefined_build_root') does not match that" \
          "of the YB_USE_NINJA env var ('$YB_USE_NINJA')"
  fi

  set_use_ninja
}

# Remove the build/latest symlink to prevent Jenkins from showing every test twice in test results.
# We call this from a few different places just in case.
remove_latest_symlink() {
  local latest_build_link=$YB_BUILD_PARENT_DIR/latest
  if [[ -h $latest_build_link ]]; then
    log "Removing the latest symlink at '$latest_build_link'"
    ( set -x; unlink "$latest_build_link" )
  fi
}

# Assigns a random "test invocation id" that allows to kill stuck processes corresponding to this
# instance of a particular test or the whole test suite.
set_test_invocation_id() {
  local timestamp=$( get_timestamp_for_filenames )
  export YB_TEST_INVOCATION_ID=test_invocation_${timestamp}_${RANDOM}_${RANDOM}_$$
}

# Kills any processes that have YB_TEST_INVOCATION_ID in their command line. Sets
# killed_stuck_processes=true in case that happens.
kill_stuck_processes() {
  expect_num_args 0 "$@"
  killed_stuck_processes=false
  if [[ -z ${YB_TEST_INVOCATION_ID:-} ]]; then
    return
  fi
  local pid
  for pid in $( pgrep -f "$YB_TEST_INVOCATION_ID" ); do
    log "Found pid $pid from this test suite (YB_TEST_INVOCATION_ID=$YB_TEST_INVOCATION_ID)," \
        "killing it with SIGKILL."
    ps -p "$pid" -f
    if kill -9 "$pid"; then
      killed_stuck_processes=true
      log "Killed process $pid with SIGKILL."
    fi
  done
}

handle_build_root_from_current_dir() {
  predefined_build_root=""
  if [[ ${YB_IS_THIRDPARTY_BUILD:-} == "1" ]]; then
    return
  fi
  local handle_predefined_build_root_quietly=true
  local d=$PWD
  while [[ $d != "/" && $d != "" ]]; do
    basename=${d##*/}
    if [[ ${YB_DEBUG_BUILD_ROOT_BASENAME_VALIDATION:-0} == "1" ]]; then
      log "Trying to match basename $basename to regex: $BUILD_ROOT_BASENAME_RE"
    fi
    if [[ $basename =~ $BUILD_ROOT_BASENAME_RE ]]; then
      predefined_build_root=$d
      handle_predefined_build_root
      return
    fi
    d=${d%/*}
  done

  fatal "Working directory of the compiler '$PWD' is not within a valid Yugabyte build root: " \
        "'$BUILD_ROOT_BASENAME_RE'"
}

validate_numeric_arg_range() {
  expect_num_args 4 "$@"
  local arg_name=$1
  local arg_value=$2
  local -r -i min_value=$3
  local -r -i max_value=$4
  if [[ ! $arg_value =~ ^[0-9]+$ ]]; then
    fatal "Invalid numeric argument value for --$arg_name: '$arg_value'"
  fi
  if [[ $arg_value -lt $min_value || $arg_value -gt $max_value ]]; then
    fatal "Value out of range for --$arg_name: $arg_value, must be between $min_value and" \
          "$max_value."
  fi
}

# -------------------------------------------------------------------------------------------------
# Python support
# -------------------------------------------------------------------------------------------------

# Checks syntax of all Python scripts in the repository.
check_python_script_syntax() {
  if [[ -n ${YB_VERBOSE:-} ]]; then
    log "Checking syntax of Python scripts"
  fi
  pushd "$YB_SRC_ROOT"
  local IFS=$'\n'
  git ls-files '*.py' | xargs -P 8 -n 1 "$YB_BUILD_SUPPORT_DIR/check_python_syntax.py"
  popd
}

add_python_wrappers_dir_to_path() {
  # Make sure the Python wrappers directory is the first on PATH
  remove_path_entry "$YB_PYTHON_WRAPPERS_DIR"
  export PATH=$YB_PYTHON_WRAPPERS_DIR:$PATH
}

activate_virtualenv() {
  local virtualenv_parent_dir=$YB_BUILD_PARENT_DIR
  local virtualenv_dir=$virtualenv_parent_dir/$YB_VIRTUALENV_BASENAME
  if [[ ! $virtualenv_dir = */$YB_VIRTUALENV_BASENAME ]]; then
    fatal "Internal error: virtualenv_dir ('$virtualenv_dir') must end" \
          "with YB_VIRTUALENV_BASENAME ('$YB_VIRTUALENV_BASENAME')"
  fi
  if [[ ${YB_RECREATE_VIRTUALENV:-} == "1" && -d $virtualenv_dir ]] && \
     ! "$yb_readonly_virtualenv"; then
    log "YB_RECREATE_VIRTUALENV is set, deleting virtualenv at '$virtualenv_dir'"
    rm -rf "$virtualenv_dir"
    unset YB_RECREATE_VIRTUALENV
  fi

  # To run pip2 itself we already need to add our Python wrappers directory to PATH.
  add_python_wrappers_dir_to_path

  if [[ ! -d $virtualenv_dir ]]; then
    if "$yb_readonly_virtualenv"; then
      fatal "virtualenv does not exist at '$virtualenv_dir', and we are not allowed to create it"
    fi
    if [[ -n ${VIRTUAL_ENV:-} && -f $VIRTUAL_ENV/bin/activate ]]; then
      local old_virtual_env=$VIRTUAL_ENV
      # Re-activate and deactivate the other virtualenv we're in. Otherwise the deactivate
      # function might not even be present in our current shell. This is necessary because otherwise
      # the --user installation below will fail.
      set +eu
      . "$VIRTUAL_ENV/bin/activate"
      deactivate
      set -eu
      # Not clear why deactivate does not do this.
      remove_path_entry "$old_virtual_env/bin"
    fi
    # We need to be using system python to install the virtualenv module or create a new virtualenv.
    (
      set -x
      pip2 install virtualenv --user
      mkdir -p "$virtualenv_parent_dir"
      cd "$virtualenv_parent_dir"
      python2 -m virtualenv "$YB_VIRTUALENV_BASENAME"
    )
  fi

  set +u
  . "$virtualenv_dir"/bin/activate
  set -u
  local pip_no_cache=""
  if [[ -n ${YB_PIP_NO_CACHE:-} ]]; then
    pip_no_cache="--no-cache-dir"
  fi

  if ! "$yb_readonly_virtualenv"; then
    local requirements_file_path="$YB_SRC_ROOT/python_requirements_frozen.txt"
    local installed_requirements_file_path=$virtualenv_dir/${requirements_file_path##*/}
    if ! cmp --silent "$requirements_file_path" "$installed_requirements_file_path"; then
      run_with_retries 10 0.5 pip2 install -r "$requirements_file_path" \
        $pip_no_cache
    fi
    # To avoid re-running pip install, save the requirements that we've installed in the virtualenv.
    cp "$requirements_file_path" "$installed_requirements_file_path"
  fi

  if [[ ${YB_DEBUG_VIRTUALENV:-0} == "1" ]]; then
    echo >&2 "
VIRTUALENV DEBUGGING
--------------------

    Activated virtualenv in: $virtualenv_dir
    Executable: $0
    PATH: $PATH
    PYTHONPATH: ${PYTHONPATH:-undefined}
    VIRTUAL_ENV: ${VIRTUAL_ENV:-undefined}

"
  fi

  export VIRTUAL_ENV
}

check_python_interpreter_version() {
  expect_num_args 3 "$@"
  local python_interpreter=$1
  local expected_major_version=$2
  local minor_version_lower_bound=$3
  # Get the Python interpreter version. Filter out debug output we may be adding if
  # YB_PYTHON_WRAPPER_DEBUG is set.
  local version_str=$(
    export yb_log_quiet=true
    "$python_interpreter" --version 2>&1 >/dev/null | grep -v "Invoking Python"
  )
  version_str=${version_str#Python }
  local actual_major_version=${version_str%%.*}
  local version_str_without_major=${version_str#*.}
  local actual_minor_version=${version_str_without_major%%.*}
  if [[ ! $actual_major_version =~ ^[0-9]+ ]]; then
    fatal "Invalid format of Python major version: $actual_major_version." \
          "Version string for interpreter $python_interpreter: $version_str"
  fi
  if [[ ! $actual_minor_version =~ ^[0-9]+ ]]; then
    fatal "Invalid format of Python minor version: $actual_minor_version." \
          "Version string for interpreter $python_interpreter: $version_str"
  fi
  if [[ $actual_major_version -ne $expected_major_version ]]; then
    fatal "Expected major version for Python interpreter '$python_interpreter' to be" \
          "'$expected_major_version', found '$actual_major_version'. Full Python version:" \
          "'$version_str'."
  fi
  if [[ $actual_minor_version -lt $minor_version_lower_bound ]]; then
    fatal "Expected minor version for Python interpreter '$python_interpreter' to be at least " \
          "'$minor_version_lower_bound', found '$actual_minor_version'. Full Python version:" \
          "'$version_str'."
  fi
}

check_python_interpreter_versions() {
  check_python_interpreter_version python2 2 7
  if is_mac; then
    local python_interpreter_basename
    for python_interpreter_basename in python python2 python 2.7 python3; do
      local homebrew_interpreter_path=/usr/local/bin/$python_interpreter_basename
      if [[ -e $homebrew_interpreter_path ]]; then
        if [[ ! -L $homebrew_interpreter_path ]]; then
          fatal "$homebrew_interpreter_path exists but is not a symlink." \
                "Broken Homebrew installation?"
        fi
        local link_target=$( readlink "$homebrew_interpreter_path" )
        if [[ $link_target == /usr/bin/* ]]; then
          fatal "Found symlink  $homebrew_interpreter_path -> $link_target." \
                "Broken Homebrew installation?"
        fi
      fi
    done
  fi
}

log_file_existence() {
  expect_num_args 1 "$@"
  local file_name=$1
  if [[ -L $file_name && -f $file_name ]]; then
    log "Symlink exists and points to a file: $file_name"
  elif [[ -L $file_name && -d $file_name ]]; then
    log "Symlink exists and points to a directory: $file_name"
  elif [[ -L $file_name ]]; then
    log "Symlink exists but it might be broken: $file_name"
  elif [[ -f $file_name ]]; then
    log "File exists: $file_name"
  elif [[ -d $file_name ]]; then
    log "Directory exists: $file_name"
  elif [[ ! -e $file_name ]]; then
    log "File does not exist: $file_name"
  else
    log "File exists but we could not determine its type: $file_name"
  fi
}

is_valid_git_sha1() {
  [[ $1 =~ ^[0-9a-f]{40} ]]
}

# Returns current git SHA1 in the variable current_git_sha1.
get_current_git_sha1() {
  current_git_sha1=$( git rev-parse HEAD )
  if ! is_valid_git_sha1 "$current_git_sha1"; then
    fatal "Could not get current git SHA1 in $PWD, got: $current_git_sha1"
  fi
}

# sed -i works differently on Linux vs macOS.
sed_i() {
  if is_mac; then
    sed -i "" "$@"
  else
    sed -i "$@"
  fi
}

lint_java_code() {
  local java_project_dir
  declare -i num_errors=0

  for java_project_dir in "${yb_java_project_dirs[@]}"; do
    local IFS=$'\n'
    local java_test_files=( $(
      find "$java_project_dir" \( -name "Test*.java" -or -name "*Test.java" \) -and \
          -not -name "TestUtils.java" -and \
          -not -name "*Base.java" -and \
          -not -name "Base*Test.java"
    ) )
    local java_test_file
    for java_test_file in "${java_test_files[@]}"; do
      local log_prefix="YB JAVA LINT: $java_test_file"
      if ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBParameterizedTestRunner\.class\)' \
             "$java_test_file" &&
         ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBTestRunner\.class\)' \
             "$java_test_file" &&
         ! grep -Eq '@RunWith\((value[ ]*=[ ]*)?YBTestRunnerNonTsanOnly\.class\)' \
             "$java_test_file"
      then
        log "$log_prefix: neither YBTestRunner, YBParameterizedTestRunner, nor" \
            "YBTestRunnerNonTsanOnly are being used in test"
        num_errors+=1
      fi
      if grep -Fq 'import static org.junit.Assert' "$java_test_file" ||
         grep -Fq 'import org.junit.Assert' "$java_test_file"; then
        log "$log_prefix: directly importing org.junit.Assert. Should use org.yb.AssertionWrappers."
        num_errors+=1
      fi
    done
  done
  if [[ $num_errors -eq 0 ]]; then
    log "Light-weight lint of YB Java code: SUCCESS"
  else
    log "Light-weight lint of YB Java code: FAILURE ($num_errors errors found)"
    return 1
  fi
}

run_with_retries() {
  if [[ $# -lt 2 ]]; then
    fatal "run_with_retries requires at least three arguments: max_attempts, delay_sec, and " \
          "the command to run (at least one additional argument)."
  fi
  declare -i -r max_attempts=$1
  declare -r delay_sec=$2
  shift 2

  declare -i attempt_index=1
  while [[ $attempt_index -le $max_attempts ]]; do
    set +e
    "$@"
    declare exit_code=$?
    set -e
    if [[ $exit_code -eq 0 ]]; then
      return
    fi
    log "Warning: command failed with exit code $exit_code at attempt $attempt_index: $*." \
        "Waiting for $delay_sec sec, will then re-try for up to $max_attempts attempts."
    let attempt_index+=1
    sleep "$delay_sec"
  done
  fatal "Failed to execute command after $max_attempts attempts: $*"
}

debug_log_boolean_function_result() {
  expect_num_args 1 "$@"
  local fn_name=$1
  if "$fn_name"; then
    log "$fn_name is true"
  else
    log "$fn_name is false"
  fi
}

set_java_home() {
  if ! is_mac; then
    return
  fi
  # macOS has a peculiar way of setting JAVA_HOME
  local cmd_to_get_java_home="/usr/libexec/java_home --version 1.8"
  local new_java_home=$( $cmd_to_get_java_home )
  if [[ ! -d $new_java_home ]]; then
    fatal "Directory returned by '$cmd_to_get_java_home' does not exist: $new_java_home"
  fi
  if [[ -n ${JAVA_HOME:-} && $JAVA_HOME != $new_java_home ]]; then
    log "Warning: updating JAVA_HOME from $JAVA_HOME to $new_java_home"
  else
    log "Setting JAVA_HOME: $new_java_home"
  fi
  export JAVA_HOME=$new_java_home
  put_path_entry_first "$JAVA_HOME/bin"
}

update_submodules() {
  # This does NOT create any new commits in the top-level repository (the "superproject").
  #
  # From documentation on "update" from https://git-scm.com/docs/git-submodule:
  # Update the registered submodules to match what the superproject expects by cloning missing
  # submodules and updating the working tree of the submodules
  ( cd "$YB_SRC_ROOT"; git submodule update --init --recursive )
}

set_prebuilt_thirdparty_url() {
  if [[ ${YB_DOWNLOAD_THIRDPARTY:-} == "1" ]]; then
    local auto_thirdparty_url=""
    local thirdparty_url_file=$YB_BUILD_SUPPORT_DIR/thirdparty_url_${short_os_name}.txt
    if [[ -f $thirdparty_url_file ]]; then
      auto_thirdparty_url=$( read_file_and_trim "$thirdparty_url_file" )
      if [[ $auto_thirdparty_url != http://* && $auto_thirdparty_url != https://* ]]; then
        fatal "Invalid third-party URL: '$auto_thirdparty_url' (expected http:// or https://)." \
              "From file: $thirdparty_url_file."
      fi
    elif [[ -z ${YB_THIRDPARTY_URL:-} ]]; then
      fatal "File $thirdparty_url_file not found, cannot set YB_THIRDPARTY_URL"
    fi

    if [[ -z ${YB_THIRDPARTY_URL:-} ]]; then
      export YB_THIRDPARTY_URL=$auto_thirdparty_url
      log "Setting third-party URL to $auto_thirdparty_url"
    elif [[ -n $auto_thirdparty_url ]]; then
      if [[ $auto_thirdparty_url != $YB_THIRDPARTY_URL ]]; then
        log "YB_THIRDPARTY_URL is already set to $YB_THIRDPARTY_URL, not trying to set it to" \
            "the default value of $auto_thirdparty_url"
      fi
    else
      fatal "YB_THIRDPARTY_URL is not set, and could not determine the default value."
    fi
  fi
}

# -------------------------------------------------------------------------------------------------
# Initialization
# -------------------------------------------------------------------------------------------------

detect_os

# http://man7.org/linux/man-pages/man7/signal.7.html
if is_mac; then
  declare -i -r SIGUSR1_EXIT_CODE=158  # 128 + 30
else
  # Linux
  declare -i -r SIGUSR1_EXIT_CODE=138  # 128 + 10
fi

# Parent directory for build directories of all build types.
YB_BUILD_INTERNAL_PARENT_DIR=$YB_SRC_ROOT/build
YB_BUILD_EXTERNAL_PARENT_DIR=${YB_SRC_ROOT}__build
if [[ ${YB_USE_EXTERNAL_BUILD_ROOT:-} == "1" ]]; then
  YB_BUILD_PARENT_DIR=$YB_BUILD_EXTERNAL_PARENT_DIR
else
  YB_BUILD_PARENT_DIR=$YB_BUILD_INTERNAL_PARENT_DIR
fi

if [[ ! -d $YB_BUILD_SUPPORT_DIR ]]; then
  fatal "Could not determine YB source directory from '$BASH_SOURCE':" \
        "$YB_BUILD_SUPPORT_DIR does not exist."
fi

if [[ -z ${YB_THIRDPARTY_DIR:-} ]]; then
  export YB_THIRDPARTY_DIR=$YB_SRC_ROOT/thirdparty
fi

readonly YB_DEFAULT_CMAKE_OPTS=(
  "-DCMAKE_C_COMPILER=$YB_COMPILER_WRAPPER_CC"
  "-DCMAKE_CXX_COMPILER=$YB_COMPILER_WRAPPER_CXX"
)

YB_PYTHON_WRAPPERS_DIR=$YB_BUILD_SUPPORT_DIR/python-wrappers

if ! "${yb_is_python_wrapper_script:-false}"; then
  detect_brew
  add_python_wrappers_dir_to_path
fi
