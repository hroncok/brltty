/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2020 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU Lesser General Public License, as published by the Free Software
 * Foundation; either version 2.1 of the License, or (at your option) any
 * later version. Please see the file LICENSE-LGPL for details.
 *
 * Web Page: http://brltty.app/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "log.h"
#include "strfmt.h"
#include "pgmprivs.h"
#include "system_linux.h"
#include "file.h"
#include "parse.h"

#define SCF_LOG_LEVEL LOG_DEBUG
#define SCF_LOG_INSTRUCTIONS 0

//#undef HAVE_PWD_H
//#undef HAVE_GRP_H
//#undef HAVE_SYS_PRCTL_H
//#undef HAVE_SYS_CAPABILITY_H
//#undef HAVE_LIBCAP
//#undef HAVE_SCHED_H
//#undef HAVE_LINUX_AUDIT_H
//#undef HAVE_LINUX_FILTER_H
//#undef HAVE_LINUX_SECCOMP_H

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif /* HAVE_SYS_PRCTL_H */

static int
amPrivilegedUser (void) {
  return !geteuid();
}

typedef struct {
  const char *reason;
  int (*install) (void);
} KernelModuleEntry;

static const KernelModuleEntry kernelModuleTable[] = {
  { .reason = "for playing alert tunes via the built-in PC speaker",
    .install = installSpeakerModule,
  },

  { .reason = "for creating virtual devices",
    .install = installUinputModule,
  },
}; static const uint8_t kernelModuleCount = ARRAY_COUNT(kernelModuleTable);

static void
installKernelModules (void) {
  const KernelModuleEntry *kme = kernelModuleTable;
  const KernelModuleEntry *end = kme + kernelModuleCount;

  while (kme < end) {
    kme->install();
    kme += 1;
  }
}

#ifdef HAVE_GRP_H
#include <grp.h>

typedef struct {
  const char *message;
  const gid_t *groups;
  size_t count;
} GroupsLogData;

static size_t
groupsLogFormatter (char *buffer, size_t size, const void *data) {
  const GroupsLogData *gld = data;

  size_t length;
  STR_BEGIN(buffer, size);
  STR_PRINTF("%s:", gld->message);

  const gid_t *gid = gld->groups;
  const gid_t *end = gid + gld->count;

  while (gid < end) {
    STR_PRINTF(" %d", *gid);

    const struct group *grp = getgrgid(*gid);
    if (grp) STR_PRINTF("(%s)", grp->gr_name);

    gid += 1;
  }

  length = STR_LENGTH;
  STR_END;
  return length;
}

static void
logGroups (int level, const char *message, const gid_t *groups, size_t count) {
  GroupsLogData gld = {
    .message = message,
    .groups = groups,
    .count = count
  };

  logData(level, groupsLogFormatter, &gld);
}

static void
logGroup (int level, const char *message, gid_t group) {
  logGroups(level, message, &group, 1);
}

typedef struct {
  const char *reason;
  const char *name;
  const char *path;
  unsigned char needRead:1;
  unsigned char needWrite:1;
} RequiredGroupEntry;

static const RequiredGroupEntry requiredGroupTable[] = {
  { .reason = "for reading screen content",
    .name = "tty",
    .path = "/dev/vcs1",
  },

  { .reason = "for virtual console monitoring and control",
    .name = "tty",
    .path = "/dev/tty1",
  },

  { .reason = "for serial I/O",
    .name = "dialout",
    .path = "/dev/ttyS0",
  },

  { .reason = "for USB I/O via USBFS",
    .path = "/dev/bus/usb",
  },

  { .reason = "for playing sound via the ALSA framework",
    .name = "audio",
    .path = "/dev/snd/seq",
  },

  { .reason = "for playing sound via the Pulse Audio daemon",
    .name = "pulse-access",
  },

  { .reason = "for monitoring keyboard input",
    .name = "input",
    .path = "/dev/input/mice",
  },

  { .reason = "for creating virtual devices",
    .path = "/dev/uinput",
    .needRead = 1,
    .needWrite = 1,
  },
}; static const uint8_t requiredGroupCount = ARRAY_COUNT(requiredGroupTable);

static void
processRequiredGroups (GroupsProcessor *processGroups, void *data) {
  gid_t groups[requiredGroupCount * 2];
  size_t count = 0;

  {
    const RequiredGroupEntry *rge = requiredGroupTable;
    const RequiredGroupEntry *end = rge + requiredGroupCount;

    while (rge < end) {
      {
        const char *name = rge->name;

        if (name) {
          const struct group *grp;

          if ((grp = getgrnam(name))) {
            groups[count++] = grp->gr_gid;
          } else {
            logMessage(LOG_WARNING, "unknown group: %s", name);
          }
        }
      }

      {
        const char *path = rge->path;

        if (path) {
          struct stat status;

          if (stat(path, &status) != -1) {
            groups[count++] = status.st_gid;

            if (rge->needRead && !(status.st_mode & S_IRGRP)) {
              logMessage(LOG_WARNING, "path not group readable: %s", path);
            }

            if (rge->needWrite && !(status.st_mode & S_IWGRP)) {
              logMessage(LOG_WARNING, "path not group writable: %s", path);
            }
          } else {
            logMessage(LOG_WARNING, "path access error: %s: %s", path, strerror(errno));
          }
        }
      }

      rge += 1;
    }
  }

  removeDuplicateGroups(groups, &count);
  processGroups(groups, count, data);
}

static void
setSupplementaryGroups (const gid_t *groups, size_t count, void *data) {
  logGroups(LOG_DEBUG, "setting supplementary groups", groups, count);

  if (setgroups(count, groups) == -1) {
    logSystemError("setgroups");
  }
}

static void
joinRequiredGroups (void) {
  processRequiredGroups(setSupplementaryGroups, NULL);
}

typedef struct {
  const gid_t *groups;
  size_t count;
} CurrentGroupsData;

static void
logUnjoinedGroups (const gid_t *groups, size_t count, void *data) {
  const CurrentGroupsData *cgd = data;

  const gid_t *cur = cgd->groups;
  const gid_t *curEnd = cur + cgd->count;

  const gid_t *req = groups;
  const gid_t *reqEnd = req + count;

  while (req < reqEnd) {
    int relation = (cur < curEnd)? compareGroups(*cur, *req): 1;

    if (relation > 0) {
      logGroup(LOG_WARNING, "group not joined", *req++);
    } else {
      if (!relation) req += 1;
      cur += 1;
    }
  }
}

static void
logWantedGroups (const gid_t *groups, size_t count, void *data) {
  CurrentGroupsData cgd = {
    .groups = groups,
    .count = count
  };

  processRequiredGroups(logUnjoinedGroups, &cgd);
}

static void
logMissingGroups (void) {
  processSupplementaryGroups(logWantedGroups, NULL);
}

static void
closeGroupsDatabase (void) {
  endgrent();
}
#endif /* HAVE_GRP_H */

#ifdef HAVE_LIBCAP
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif /* HAVE_SYS_CAPABILITY_H */
#endif /* HAVE_LIBCAP */

#ifdef CAP_IS_SUPPORTED
typedef struct {
  const char *label;
  cap_t caps;
} CapabilitiesLogData;

static size_t
capabilitiesLogFormatter (char *buffer, size_t size, const void *data) {
  const CapabilitiesLogData *cld = data;

  size_t length;
  STR_BEGIN(buffer, size);
  STR_PRINTF("capabilities: %s:", cld->label);

  int capsAllocated = 0;
  cap_t caps;

  if (!(caps = cld->caps)) {
    if (!(caps = cap_get_proc())) {
      logSystemError("cap_get_proc");
      goto done;
    }

    capsAllocated = 1;
  }

  {
    char *text;

    if ((text = cap_to_text(caps, NULL))) {
      STR_PRINTF(" %s", text);
      cap_free(text);
    } else {
      logSystemError("cap_to_text");
    }
  }

  if (capsAllocated) {
    cap_free(caps);
    caps = NULL;
  }

done:
  length = STR_LENGTH;
  STR_END;
  return length;
}

static void
logCapabilities (cap_t caps, const char *label) {
  CapabilitiesLogData cld = { .label=label, .caps=caps };
  logData(LOG_DEBUG, capabilitiesLogFormatter, &cld);
}

static void
logCurrentCapabilities (const char *label) {
  logCapabilities(NULL, label);
}

static int
setCapabilities (cap_t caps) {
  if (cap_set_proc(caps) != -1) return 1;
  logSystemError("cap_set_proc");
  return 0;
}

static int
hasCapability (cap_t caps, cap_flag_t set, cap_value_t capability) {
  cap_flag_value_t value;
  if (cap_get_flag(caps, capability, set, &value) != -1) return value == CAP_SET;
  logSystemError("cap_get_flag");
  return 0;
}

static int
addCapability (cap_t caps, cap_flag_t set, cap_value_t capability) {
  if (cap_set_flag(caps, set, 1, &capability, CAP_SET) != -1) return 1;
  logSystemError("cap_set_flag");
  return 0;
}

typedef struct {
  const char *reason;
  cap_value_t value;
} RequiredCapabilityEntry;

static const RequiredCapabilityEntry requiredCapabilityTable[] = {
  { .reason = "for injecting input characters typed on a braille device",
    .value = CAP_SYS_ADMIN,
  },

  { .reason = "for playing alert tunes via the built-in PC speaker",
    .value = CAP_SYS_TTY_CONFIG,
  },

  { .reason = "for creating needed but missing special device files",
    .value = CAP_MKNOD,
  },
}; static const uint8_t requiredCapabilityCount = ARRAY_COUNT(requiredCapabilityTable);

static void
setRequiredCapabilities (void) {
  cap_t newCaps, oldCaps;

  if (amPrivilegedUser()) {
    oldCaps = NULL;
  } else if (!(oldCaps = cap_get_proc())) {
    logSystemError("cap_get_proc");
    return;
  }

  if ((newCaps = cap_init())) {
    {
      const RequiredCapabilityEntry *rce = requiredCapabilityTable;
      const RequiredCapabilityEntry *end = rce + requiredCapabilityCount;

      while (rce < end) {
        cap_value_t capability = rce->value;

        if (!oldCaps || hasCapability(oldCaps, CAP_PERMITTED, capability)) {
          if (!addCapability(newCaps, CAP_PERMITTED, capability)) break;
          if (!addCapability(newCaps, CAP_EFFECTIVE, capability)) break;
        }

        rce += 1;
      }
    }

    setCapabilities(newCaps);
    cap_free(newCaps);
  } else {
    logSystemError("cap_init");
  }

#ifdef PR_CAP_AMBIENT_CLEAR_ALL
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == -1) {
    logSystemError("prctl[PR_CAP_AMBIENT_CLEAR_ALL]");
  }
#else /* PR_CAP_AMBIENT_CLEAR_ALL */
  logMessage(LOG_WARNING, "can't clear ambient capabilities");
#endif /* PR_CAP_AMBIENT_CLEAR_ALL */

  if (oldCaps) cap_free(oldCaps);
}

static void
logMissingCapabilities (void) {
  cap_t caps;

  if ((caps = cap_get_proc())) {
    const RequiredCapabilityEntry *rce = requiredCapabilityTable;
    const RequiredCapabilityEntry *end = rce + requiredCapabilityCount;

    while (rce < end) {
      cap_value_t capability = rce->value;

      if (!hasCapability(caps, CAP_EFFECTIVE, capability)) {
        logMessage(LOG_WARNING,
          "required capability not granted: %s (%s)",
          cap_to_name(capability), rce->reason
        );
      }

      rce += 1;
    }

    cap_free(caps);
  } else {
    logSystemError("cap_get_proc");
  }
}

static int
requestCapability (cap_t caps, cap_value_t capability, int inheritable) {
  if (!hasCapability(caps, CAP_EFFECTIVE, capability)) {
    if (!hasCapability(caps, CAP_PERMITTED, capability)) {
      logMessage(LOG_WARNING, "capability not permitted: %s", cap_to_name(capability));
      return 0;
    }

    if (!addCapability(caps, CAP_EFFECTIVE, capability)) return 0;
    if (!inheritable) return setCapabilities(caps);
  } else if (!inheritable) {
    return 1;
  }

  if (!hasCapability(caps, CAP_INHERITABLE, capability)) {
    if (!addCapability(caps, CAP_INHERITABLE, capability)) {
      return 0;
    }
  }

  if (setCapabilities(caps)) {
#ifdef PR_CAP_AMBIENT_RAISE
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, capability, 0, 0) != -1) return 1;
    logSystemError("prctl[PR_CAP_AMBIENT_RAISE]");
#else /* PR_CAP_AMBIENT_RAISE */
    logMessage(LOG_WARNING, "can't raise ambient capabilities");
#endif /* PR_CAP_AMBIENT_RAISE */
  }

  return 0;
}

static int
needCapability (cap_value_t capability, int inheritable, const char *reason) {
  int haveCapability = 0;
  const char *outcome = NULL;
  cap_t caps;

  if ((caps = cap_get_proc())) {
    if (hasCapability(caps, CAP_EFFECTIVE, capability)) {
      haveCapability = 1;
      outcome = "already added";
    } else if (requestCapability(caps, capability, inheritable)) {
      haveCapability = 1;
      outcome = "added";
    } else {
      outcome = "not granted";
    }

    cap_free(caps);
  } else {
    logSystemError("cap_get_proc");
  }

  if (outcome) {
    logMessage(LOG_DEBUG,
      "temporary capability %s: %s (%s)",
      outcome, cap_to_name(capability), reason
    );
  }

  return haveCapability;
}

#else /* CAP_IS_SUPPORTED */
static void
logCurrentCapabilities (const char *label) {
}
#endif /* CAP_IS_SUPPORTED */

#ifdef HAVE_SCHED_H
#include <sched.h>

typedef struct {
  const char *name;
  const char *summary;
  int unshareFlag;
} IsolatedNamespaceEntry;

static const IsolatedNamespaceEntry isolatedNamespaceTable[] = {
  #ifdef CLONE_NEWCGROUP
  { .unshareFlag = CLONE_NEWCGROUP,
    .name = "cgroup",
    .summary = "control groups",
  },
  #endif /* CLONE_NEWCGROUP */

  #ifdef CLONE_NEWIPC
  { .unshareFlag = CLONE_NEWIPC,
    .name = "IPC",
    .summary = "System V interprocess communication objects and POSIX message queues",
  },
  #endif /* CLONE_NEWIPC */

  #ifdef CLONE_NEWNS
  { .unshareFlag = CLONE_NEWNS,
    .name = "mount",
    .summary = "mount points",
  },
  #endif /* CLONE_NEWNS */

  #ifdef CLONE_NEWUTS
  { .unshareFlag = CLONE_NEWUTS,
    .name = "UTS",
    .summary = "host name and NIS domain name",
  },
  #endif /* CLONE_NEWUTS */
}; static const uint8_t isolatedNamespaceCount = ARRAY_COUNT(isolatedNamespaceTable);

static void
isolateNamespaces (void) {
  int canIsolateNamespaces = 0;

#ifdef CAP_SYS_ADMIN
  if (needCapability(CAP_SYS_ADMIN, 0, "for isolating namespaces")) {
    canIsolateNamespaces = 1;
  }
#endif /* CAP_SYS_ADMIN */

  if (canIsolateNamespaces) {
    int unshareFlags = 0;

    const IsolatedNamespaceEntry *ine = isolatedNamespaceTable;
    const IsolatedNamespaceEntry *end = ine + isolatedNamespaceCount;

    while (ine < end) {
      logMessage(LOG_DEBUG,
        "isolating namespace: %s (%s)", ine->name, ine->summary
      );

      unshareFlags |= ine->unshareFlag;
      ine += 1;
    }

    if (unshare(unshareFlags) == -1) {
      logSystemError("unshare");
    }
  } else {
    logMessage(LOG_DEBUG, "can't isolate namespaces");
  }
}
#endif /* HAVE_SCHED_H */

#ifdef HAVE_LINUX_AUDIT_H
#include <linux/audit.h>

#if defined(__i386__)
#define SCF_ARCHITECTURE AUDIT_ARCH_I386
#elif defined(__x86_64__)
#define SCF_ARCHITECTURE AUDIT_ARCH_X86_64
#else /* security filter architecture */
#warning seccomp not supported on this platform
#endif /* security filter architecture */
#endif /* HAVE_LINUX_AUDIT_H */

#ifdef SCF_ARCHITECTURE
#ifdef HAVE_LINUX_FILTER_H
#include <linux/filter.h>

#ifdef HAVE_LINUX_SECCOMP_H
#include <linux/seccomp.h>
#endif /* HAVE_LINUX_SECCOMP_H */
#endif /* HAVE_LINUX_FILTER_H */
#endif /* SCF_ARCHITECTURE */

#ifdef SECCOMP_MODE_FILTER
typedef struct SCFTableEntryStruct SCFTableEntry;

struct SCFTableEntryStruct {
  uint32_t value;

  struct {
    const SCFTableEntry *table;
    uint8_t index;
  } argument;
};

#define SCF_BEGIN_TABLE(name) static const SCFTableEntry name##Table[] = {
#define SCF_END_TABLE {UINT32_MAX}};
#include "syscalls_linux.h"

#define SCF_FIELD_OFFSET(field) offsetof(struct seccomp_data, field)

#define SCF_RETURN(action, value) \
BPF_STMT(BPF_RET|BPF_K, (SECCOMP_RET_##action | ((value) & SECCOMP_RET_DATA)))

typedef enum {
  SCF_JUMP_ALWAYS,
  SCF_JUMP_TRUE,
  SCF_JUMP_FALSE
} SCFJumpType;

typedef struct SCFJumpStruct SCFJump;

struct SCFJumpStruct {
  SCFJump *next;
  size_t location;
  SCFJumpType type;
};

typedef struct {
  SCFJump jump;
  const SCFTableEntry *table;
  uint8_t index;
} SCFArgumentTable;

typedef struct {
  struct {
    struct sock_filter *array;
    size_t size;
    size_t count;
  } instruction;

  struct {
    SCFJump *allow;
  } jumps;

  struct {
    SCFArgumentTable *array;
    size_t size;
    size_t count;
  } subtable;
} SCFObject;

static SCFObject *
scfNewObject (void) {
  SCFObject *scf;

  if ((scf = malloc(sizeof(*scf)))) {
    memset(scf, 0, sizeof(*scf));

    scf->instruction.array = NULL;
    scf->instruction.size = 0;
    scf->instruction.count = 0;

    scf->jumps.allow = NULL;

    return scf;
  } else {
    logMallocError();
  }

  return NULL;
}

static void
scfDestroyJumps (SCFJump *jump) {
  while (jump) {
    SCFJump *next = jump->next;
    free(jump);
    jump = next;
  }
}

static void
scfDestroyObject (SCFObject *scf) {
  scfDestroyJumps(scf->jumps.allow);
  if (scf->instruction.array) free(scf->instruction.array);
  if (scf->subtable.array) free(scf->subtable.array);
  free(scf);
}

static int
scfAddInstruction (SCFObject *scf, const struct sock_filter *instruction) {
  if (scf->instruction.count == BPF_MAXINSNS) {
    logMessage(LOG_ERR, "syscall filter too large");
    return 0;
  }

  if (scf->instruction.count == scf->instruction.size) {
    size_t newSize = scf->instruction.size? scf->instruction.size<<1: 0X10;
    struct sock_filter *newArray;

    if (!(newArray = realloc(scf->instruction.array, ARRAY_SIZE(newArray, newSize)))) {
      logMallocError();
      return 0;
    }

    scf->instruction.array = newArray;
    scf->instruction.size = newSize;
  }

  scf->instruction.array[scf->instruction.count++] = *instruction;
  return 1;
}

static int
scfLoadField (SCFObject *scf, uint32_t offset, uint8_t width) {
  struct sock_filter instruction = BPF_STMT(BPF_LD|BPF_ABS, offset);

  switch (width) {
    case 1:
      instruction.code |= BPF_B;
      break;

    case 2:
      instruction.code |= BPF_H;
      break;

    case 4:
      instruction.code |= BPF_W;
      break;

    default:
      logMessage(LOG_ERR, "unsupported field width: %u", width);
      return 0;
  }

  return scfAddInstruction(scf, &instruction);
}

static int
scfLoadArchitecture (SCFObject *scf) {
  return scfLoadField(scf, SCF_FIELD_OFFSET(arch), 4);
}

static int
scfLoadSyscall (SCFObject *scf) {
  return scfLoadField(scf, SCF_FIELD_OFFSET(nr), 4);
}

static int
scfLoadArgument (SCFObject *scf, uint8_t index) {
  return scfLoadField(scf, SCF_FIELD_OFFSET(args[index]), 4);
}

static void
scfBeginJump (SCFObject *scf, SCFJump *jump, SCFJumpType type) {
  memset(jump, 0, sizeof(*jump));
  jump->next = NULL;
  jump->location = scf->instruction.count;
  jump->type = type;
}

static int
scfEndJump (SCFObject *scf, const SCFJump *jump) {
  size_t from = jump->location;
  struct sock_filter *instruction = &scf->instruction.array[from];

  size_t to = scf->instruction.count - from - 1;
  SCFJumpType type = jump->type;

  switch (type) {
    case SCF_JUMP_ALWAYS:
      instruction->k = to;
      break;

    case SCF_JUMP_TRUE:
      instruction->jt = to;
      break;

    case SCF_JUMP_FALSE:
      instruction->jf = to;
      break;

    default:
      logMessage(LOG_ERR, "unsupported jump type: %u", type);
      return 0;
  }

  return 1;
}

static int
scfEndJumps (SCFObject *scf, SCFJump **jumps) {
  int ok = 1;

  while (*jumps) {
    SCFJump *jump = *jumps;
    *jumps = jump->next;

    if (!scfEndJump(scf, jump)) ok = 0;
    free(jump);
  }

  return ok;
}

static int
scfJumpTo (SCFObject *scf, SCFJump *jump) {
  struct sock_filter instruction = BPF_STMT(BPF_JMP|BPF_K|BPF_JA, 0);
  scfBeginJump(scf, jump, SCF_JUMP_ALWAYS);
  return scfAddInstruction(scf, &instruction);
}

typedef enum {
  SCF_TEST_NE,
  SCF_TEST_LT,
  SCF_TEST_LE,
  SCF_TEST_EQ,
  SCF_TEST_GE,
  SCF_TEST_GT,
} SCFTest;

static int
scfJumpIf (SCFObject *scf, SCFJump *jump, uint32_t value, SCFTest test) {
  struct sock_filter instruction = BPF_STMT(BPF_JMP|BPF_K, value);
  int invert = 0;

  switch (test) {
    case SCF_TEST_NE:
      invert = 1;
    case SCF_TEST_EQ:
      instruction.code |= BPF_JEQ;
      break;

    case SCF_TEST_LT:
      invert = 1;
    case SCF_TEST_GE:
      instruction.code |= BPF_JGE;
      break;

    case SCF_TEST_LE:
      invert = 1;
    case SCF_TEST_GT:
      instruction.code |= BPF_JGT;
      break;

    default:
      logMessage(LOG_ERR, "unsupported value test: %u", test);
      return 0;
  }

  scfBeginJump(scf, jump, (invert? SCF_JUMP_FALSE: SCF_JUMP_TRUE));
  return scfAddInstruction(scf, &instruction);
}

static int
scfVerifyArchitecture (SCFObject *scf, const struct sock_filter *deny) {
  SCFJump jump;

  return scfLoadArchitecture(scf)
      && scfJumpIf(scf, &jump, SCF_ARCHITECTURE, SCF_TEST_EQ)
      && scfAddInstruction(scf, deny)
      && scfEndJump(scf, &jump);
}

static int
scfAddArgumentTable (SCFObject *scf, uint8_t index, const SCFTableEntry *table) {
  if (scf->subtable.count == scf->subtable.size) {
    size_t newSize = scf->subtable.size? scf->subtable.size<<1: 0X10;
    SCFArgumentTable *newArray;

    if (!(newArray = realloc(scf->subtable.array, ARRAY_SIZE(newArray, newSize)))) {
      logMallocError();
      return 0;
    }

    scf->subtable.array = newArray;
    scf->subtable.size = newSize;
  }

  SCFArgumentTable argument = {
    .index = index,
    .table = table
  };

  if (!scfJumpTo(scf, &argument.jump)) return 0;
  scf->subtable.array[scf->subtable.count++] = argument;
  return 1;
}

static int
scfAllowTableEntry (SCFObject *scf, const SCFTableEntry *entry) {
  if (entry->argument.table) {
    SCFJump jump;

    return scfJumpIf(scf, &jump, entry->value, SCF_TEST_NE)
        && scfAddArgumentTable(scf, entry->argument.index, entry->argument.table)
        && scfEndJump(scf, &jump);
  } else {
    SCFJump *jump;

    if ((jump = malloc(sizeof(*jump)))) {
      if (scfJumpIf(scf, jump, entry->value, SCF_TEST_EQ)) {
        jump->next = scf->jumps.allow;
        scf->jumps.allow = jump;
        return 1;
      }

      free(jump);
    } else {
      logMallocError();
    }
  }

  return 0;
}

static int
scfAllowTableEntries (SCFObject *scf, const SCFTableEntry *entries, size_t count, const struct sock_filter *deny) {
  if (count <= 3) {
    const SCFTableEntry *cur = entries;
    const SCFTableEntry *end = cur + count;

    while (cur < end) {
      if (!scfAllowTableEntry(scf, cur)) return 0;
      cur += 1;
    }

    return scfAddInstruction(scf, deny);
  }

  const SCFTableEntry *entry = entries + (count / 2);
  SCFJump jump;
  if (!scfJumpIf(scf, &jump, entry->value, SCF_TEST_GT)) return 0;
  if (!scfAllowTableEntry(scf, entry)) return 0;

  if (!scfAllowTableEntries(scf, entries, (entry - entries), deny)) return 0;
  if (!scfEndJump(scf, &jump)) return 0;

  const SCFTableEntry *end = entries + count;
  entry += 1;
  return scfAllowTableEntries(scf, entry, (end - entry), deny);
}

static size_t
scfGetTableEntryCount (const SCFTableEntry *table) {
  const SCFTableEntry *end = table;
  while (end->value != UINT32_MAX) end += 1;
  return end - table;
}

static int
scfTableEntrySorter (const void *element1, const void *element2) {
  const SCFTableEntry *entry1 = element1;
  const SCFTableEntry *entry2 = element2;

  if (entry1->value < entry2->value) return -1;
  if (entry1->value > entry2->value) return 1;
  return 0;
}

static int
scfAllowTable (SCFObject *scf, const SCFTableEntry *table, const struct sock_filter *deny) {
  {
    size_t count = scfGetTableEntryCount(table);
    SCFTableEntry entries[count];
    memcpy(entries, table, sizeof(entries));
    qsort(entries, count, sizeof(entries[0]), scfTableEntrySorter);
    if (!scfAllowTableEntries(scf, entries, count, deny)) return 0;
  }

  if (scf->jumps.allow) {
    if (!scfEndJumps(scf, &scf->jumps.allow)) return 0;
    static const struct sock_filter allow = SCF_RETURN(ALLOW, 0);
    if (!scfAddInstruction(scf, &allow)) return 0;
  }

  return 1;
}

static int
scfAllowSyscallTable (SCFObject *scf, const struct sock_filter *deny) {
  if (!scfLoadSyscall(scf)) return 0;
  return scfAllowTable(scf, syscallTable, deny);
}

static int
scfAllowArgumentTables (SCFObject *scf, const struct sock_filter *deny) {
  for (size_t index=0; index<scf->subtable.count; index+=1) {
    const SCFArgumentTable *argument = &scf->subtable.array[index];
    if (!scfEndJump(scf, &argument->jump)) return 0;
    if (!scfLoadArgument(scf, argument->index)) return 0;
    if (!scfAllowTable(scf, argument->table, deny)) return 0;
  }

  return 1;
}

#if SCF_LOG_INSTRUCTIONS
static size_t
scfDisassembleInstructionCode (uint16_t code, uint32_t value, char *buffer, size_t size) {
  size_t length;
  STR_BEGIN(buffer, size);

  const char *name = NULL;
  int hasSize = 0;
  int hasMode = 0;
  int hasSource = 0;
  int isReturn = 0;
  int problem = 0;

  switch (BPF_CLASS(code)) {
    case BPF_LD:
      name = "ld";
      hasSize = 1;
      hasMode = 1;
      break;

    case BPF_LDX:
      name = "ldx";
      hasSize = 1;
      hasMode = 1;
      break;

    case BPF_ST:
      name = "st";
      hasSize = 1;
      hasMode = 1;
      break;

    case BPF_STX:
      name = "stx";
      hasSize = 1;
      hasMode = 1;
      break;

    case BPF_ALU:
      switch (BPF_OP(code)) {
        case BPF_ADD:
          name = "add";
          break;

        case BPF_SUB:
          name = "sub";
          break;

        case BPF_MUL:
          name = "mul";
          break;

        case BPF_DIV:
          name = "div";
          break;

        case BPF_MOD:
          name = "mod";
          break;

        case BPF_LSH:
          name = "lsh";
          break;

        case BPF_RSH:
          name = "rsh";
          break;

        case BPF_AND:
          name = "and";
          break;

        case BPF_OR:
          name = "or";
          break;

        case BPF_XOR:
          name = "xor";
          break;

        case BPF_NEG:
          name = "neg";
          break;

        default:
          name = "alu";
          problem = 1;
          break;
      }

      hasSource = 1;
      break;

    case BPF_JMP:
      switch (BPF_OP(code)) {
        case BPF_JEQ:
          name = "jeq";
          break;

        case BPF_JGT:
          name = "jgt";
          break;

        case BPF_JGE:
          name = "jge";
          break;

        case BPF_JSET:
          name = "jseT";
          break;

        default:
          problem = 1;
          /* fall through */

        case BPF_JA:
          name = "jmp";
          break;
      }

      hasSource = 1;
      break;

    case BPF_RET:
      name = "ret";
      isReturn = 1;
      break;

    default:
      problem = 1;
      break;
  }

  if (name) STR_PRINTF("%s", name);

  if (hasSize) {
    const char *size = NULL;

    switch (BPF_SIZE(code)) {
      case BPF_B:
        size = "b";
        break;

      case BPF_H:
        size = "h";
        break;

      case BPF_W:
        size = "w";
        break;

      default:
        problem = 1;
        break;
    }

    if (size) STR_PRINTF("-%s", size);
  }

  if (hasMode) {
    const char *mode = NULL;

    switch (BPF_MODE(code)) {
      case BPF_IMM:
        mode = "imm";
        break;

      case BPF_ABS:
        mode = "abs";
        break;

      case BPF_IND:
        mode = "ind";
        break;

      case BPF_MEM:
        mode = "mem";
        break;

      case BPF_LEN:
        mode = "len";
        break;

      default:
        problem = 1;
        break;
    }

    if (mode) STR_PRINTF("-%s", mode);
  }

  if (hasSource) {
    const char *source = NULL;

    switch (BPF_SRC(code)) {
      case BPF_K:
        source = "k";
        break;

      case BPF_X:
        source = "x";
        break;

      default:
        problem = 1;
        break;
    }

    if (source) STR_PRINTF("-%s", source);
  }

  if (isReturn) {
    const char *action = NULL;

    switch (value & SECCOMP_RET_ACTION_FULL) {
      case SECCOMP_RET_KILL_PROCESS:
        action = "kill-process";
        break;

      case SECCOMP_RET_KILL_THREAD:
        action = "kill-thread";
        break;

      case SECCOMP_RET_TRAP:
        action = "trap";
        break;

      case SECCOMP_RET_ERRNO:
        action = "errno";
        break;

      case SECCOMP_RET_USER_NOTIF:
        action = "notify";
        break;

      case SECCOMP_RET_TRACE:
        action = "trace";
        break;

      case SECCOMP_RET_LOG:
        action = "log";
        break;

      case SECCOMP_RET_ALLOW:
        action = "allow";
        break;

      default:
        break;
    }

    if (action) {
      STR_PRINTF("-%s", action);
      uint16_t data = value & SECCOMP_RET_DATA;
      if (data) STR_PRINTF("(%u)", data);
    }
  }

  if (problem) STR_PRINTF("?");
  length = STR_LENGTH;
  STR_END;
  return length;
}
#endif /* SCF_LOG_INSTRUCTIONS */

static void
scfLogInstructions (SCFObject *scf) {
  const  char *label = "SCF";

  size_t count = scf->instruction.count;
  logMessage(SCF_LOG_LEVEL, "%s instruction count: %zu", label, count);

#if SCF_LOG_INSTRUCTIONS
  {
    int decIndexWidth;
    int hexIndexWidth;

    {
      size_t index = count;
      if (index > 0) index -= 1;

      char buffer[0X40];
      decIndexWidth = snprintf(buffer, sizeof(buffer), "%zu", index);
      hexIndexWidth = snprintf(buffer, sizeof(buffer), "%zx", index);
    }

    for (size_t index=0; index<count; index+=1) {
      const struct sock_filter *instruction = &scf->instruction.array[index];

      char disassembly[0X40];
      scfDisassembleInstructionCode(
        instruction->code, instruction->k,
        disassembly, sizeof(disassembly)
      );

      logMessage(SCF_LOG_LEVEL,
        "%s: %*zu %0*zX: %04X %08X %02X %02X: %s",
        label, decIndexWidth, index, hexIndexWidth, index,
        instruction->code, instruction->k,
        instruction->jt, instruction->jf,
        disassembly
      );
    }
  }
#endif /* SCF_LOG_INSTRUCTIONS */
}

static const struct sock_filter *
getDenyInstruction (const char *modeKeyword) {
  typedef struct {
    const char *keyword;
    struct sock_filter instruction;
  } ModeEntry;

  static const ModeEntry modeTable[] = {
    #ifdef SECCOMP_RET_ALLOW
    { .keyword = "no",
      .instruction = SCF_RETURN(ALLOW, 0)
    },
    #endif /* SECCOMP_RET_ALLOW */

    #ifdef SECCOMP_RET_LOG
    { .keyword = "log",
      .instruction = SCF_RETURN(LOG, 0)
    },
    #endif /* SECCOMP_RET_LOG */

    #ifdef SECCOMP_RET_ERRNO
    { .keyword = "fail",
      .instruction = SCF_RETURN(ERRNO, EPERM)
    },
    #endif /* SECCOMP_RET_ERRNO */

    #ifdef SECCOMP_RET_KILL_PROCESS
    { .keyword = "kill",
      .instruction = SCF_RETURN(KILL_PROCESS, 0)
    },
    #endif /* SECCOMP_RET_KILL_PROCESS */

    { .keyword = NULL }
  };

  unsigned int choice;
  int valid = validateChoiceEx(&choice, modeKeyword, modeTable, sizeof(modeTable[0]));
  const ModeEntry *mode = &modeTable[choice];

  if (!valid) {
    logMessage(LOG_WARNING,
      "unknown SECCOMP mode: %s: assuming %s",
      modeKeyword, mode->keyword
    );
  }

  return &mode->instruction;
}

static SCFObject *
makeSecureComputingFilter (const struct sock_filter *deny) {
  SCFObject *scf;

  if ((scf = scfNewObject())) {
    if (scfVerifyArchitecture(scf, deny)) {
      if (scfAllowSyscallTable(scf, deny)) {
        if (scfAllowArgumentTables(scf, deny)) {
          scfLogInstructions(scf);
          return scf;
        }
      }
    }

    scfDestroyObject(scf);
  }

  return NULL;
}

static void
installSecureComputingFilter (const char *modeKeyword) {
  const struct sock_filter *deny = getDenyInstruction(modeKeyword);

  if ((deny->k & SECCOMP_RET_ACTION_FULL) != SECCOMP_RET_ALLOW) {
    SCFObject *scf;

    if ((scf = makeSecureComputingFilter(deny))) {
      struct sock_fprog program = {
        .filter = scf->instruction.array,
        .len = scf->instruction.count
      };

#if defined(PR_SET_SECCOMP)
      if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == -1) {
        logSystemError("prctl[PR_SET_SECCOMP,SECCOMP_MODE_FILTER]");
      }
#elif defined(SECCOMP_SET_MODE_FILTER)
      unsigned int flags = 0;

      if (seccomp(SECCOMP_SET_MODE_FILTER, flags, &program) == -1) {
        logSystemError("seccomp[SECCOMP_SET_MODE_FILTER]");
      }
#else /* install secure computing filter */
#warning no mechanism for installing the secure computing filter
#endif /* install secure computing filter */

      scfDestroyObject(scf);
    }
  }
}
#endif /* SECCOMP_MODE_FILTER */

typedef void PrivilegesAcquisitionFunction (void);
typedef void MissingPrivilegesLogger (void);
typedef void ReleaseResourcesFunction (void);

typedef struct {
  const char *reason;
  PrivilegesAcquisitionFunction *acquirePrivileges;
  MissingPrivilegesLogger *logMissingPrivileges;
  ReleaseResourcesFunction *releaseResources;

  #ifdef CAP_IS_SUPPORTED
  cap_value_t capability;
  unsigned char inheritable:1;
  #endif /* CAP_IS_SUPPORTED */
} PrivilegesAcquisitionEntry;

static const PrivilegesAcquisitionEntry privilegesAcquisitionTable[] = {
  { .reason = "for installing kernel modules",
    .acquirePrivileges = installKernelModules,

    #ifdef CAP_SYS_MODULE
    .capability = CAP_SYS_MODULE,
    .inheritable = 1,
    #endif /* CAP_SYS_MODULE, */
  },

#ifdef HAVE_GRP_H
  { .reason = "for joining the required groups",
    .acquirePrivileges = joinRequiredGroups,
    .logMissingPrivileges = logMissingGroups,
    .releaseResources = closeGroupsDatabase,

    #ifdef CAP_SETGID
    .capability = CAP_SETGID,
    #endif /* CAP_SETGID, */
  },
#endif /* HAVE_GRP_H */

// This one must be last because it relinquishes the temporary capabilities.
#ifdef CAP_IS_SUPPORTED
  { .reason = "for assigning required capabilities",
    .acquirePrivileges = setRequiredCapabilities,
    .logMissingPrivileges = logMissingCapabilities,
  }
#endif /* CAP_IS_SUPPORTED */
}; static const uint8_t privilegesAcquisitionCount = ARRAY_COUNT(privilegesAcquisitionTable);

static void
acquirePrivileges (void) {
  if (amPrivilegedUser()) {
    const PrivilegesAcquisitionEntry *pae = privilegesAcquisitionTable;
    const PrivilegesAcquisitionEntry *end = pae + privilegesAcquisitionCount;

    while (pae < end) {
      pae->acquirePrivileges();
      pae += 1;
    }
  }

#ifdef CAP_IS_SUPPORTED
  else {
    const PrivilegesAcquisitionEntry *pae = privilegesAcquisitionTable;
    const PrivilegesAcquisitionEntry *end = pae + privilegesAcquisitionCount;

    while (pae < end) {
      cap_value_t capability = pae->capability;

      if (!capability || needCapability(capability, pae->inheritable, pae->reason)) {
        pae->acquirePrivileges();
      }

      pae += 1;
    }
  }
#endif /* CAP_IS_SUPPORTED */

  {
    const PrivilegesAcquisitionEntry *pae = privilegesAcquisitionTable;
    const PrivilegesAcquisitionEntry *end = pae + privilegesAcquisitionCount;

    while (pae < end) {
      {
        MissingPrivilegesLogger *log = pae->logMissingPrivileges;
        if (log) log();
      }

      {
        ReleaseResourcesFunction *release = pae->releaseResources;
        if (release) release();
      }

      pae += 1;
    }
  }
}

static int
setEnvironmentVariable (const char *name, const char *value) {
  if (setenv(name, value, 1) != -1) {
    logMessage(LOG_DEBUG, "environment variable set: %s: %s", name, value);
    return 1;
  } else {
    logSystemError("setenv");
  }

  return 0;
}

static int
setHomeDirectory (const char *directory) {
  if (!directory) return 0;
  if (!*directory) return 0;

  if (chdir(directory) != -1) {
    logMessage(LOG_DEBUG, "working directory changed: %s", directory);
    setEnvironmentVariable("HOME", directory);
    return 1;
  } else {
    logMessage(LOG_WARNING, 
      "working directory not changed: %s: %s",
      directory, strerror(errno)
    );
  }

  return 0;
}

static int
setCommandSearchPath (const char *path) {
  const char *variable = "PATH";

  if (!*path) {
    int parameter = _CS_PATH;
    size_t size = confstr(parameter, NULL, 0);

    if (size > 0) {
      char buffer[size];
      confstr(parameter, buffer, sizeof(buffer));
      return setEnvironmentVariable(variable, buffer);
    }

    path = "/usr/sbin:/sbin:/usr/bin:/bin";
  }

  return setEnvironmentVariable(variable, path);
}

static int
setDefaultShell (const char *shell) {
  if (!*shell) shell = "/bin/sh";
  return setEnvironmentVariable("SHELL", shell);
}

#ifdef HAVE_PWD_H
#include <pwd.h>

static int
canSwitchGroup (gid_t gid) {
  {
    gid_t rGid, eGid, sGid;
    getresgid(&rGid, &eGid, &sGid);
    if ((gid == rGid) || (gid == eGid) || (gid == sGid)) return 1;
  }

#ifdef CAP_SETGID
  if (needCapability(CAP_SETGID, 0, "for switching to the writable group")) {
    return 1;
  }
#endif /* CAP_SETGID */

  return 0;
}

static int
setProcessOwnership (uid_t uid, gid_t gid) {
  gid_t oldRgid, oldEgid, oldSgid;

  if (getresgid(&oldRgid, &oldEgid, &oldSgid) != -1) {
    if (setresgid(gid, gid, gid) != -1) {
      if (setresuid(uid, uid, uid) != -1) {
        return 1;
      } else {
        logSystemError("setresuid");
      }

      setresgid(oldRgid, oldEgid, oldSgid);
    } else {
      logSystemError("setresgid");
    }
  } else {
    logSystemError("getresgid");
  }

  return 0;
}

static int
switchToUser (const char *user, int *haveHomeDirectory) {
  const struct passwd *pwd;

  if ((pwd = getpwnam(user))) {
    uid_t uid = pwd->pw_uid;
    gid_t gid = pwd->pw_gid;

    if (!uid) {
      logMessage(LOG_WARNING, "not an unprivileged user: %s", user);
    } else if (setProcessOwnership(uid, gid)) {
      logMessage(LOG_NOTICE, "switched to user: %s", user);
      if (setHomeDirectory(pwd->pw_dir)) *haveHomeDirectory = 1;
      return 1;
    }
  } else {
    logMessage(LOG_WARNING, "user not found: %s", user);
  }

  return 0;
}

static int
switchUser (const char *specifiedUser, const char *configuredUser, int *haveHomeDirectory) {
  const char *user;

  if (amPrivilegedUser()) {
    if (strcmp(specifiedUser, configuredUser) != 0) {
      if (*(user = specifiedUser)) {
        if (switchToUser(user, haveHomeDirectory)) {
          return 1;
        }
      }
    }

    if (*(user = configuredUser)) {
      if (switchToUser(user, haveHomeDirectory)) return 1;
      logMessage(LOG_WARNING, "couldn't switch to the configured unprivileged user: %s", user);
    } else {
      logMessage(LOG_WARNING, "unprivileged user not configured");
    }
  }

  {
    uid_t uid = getuid();
    gid_t gid = getgid();

    {
      const struct passwd *pwd;
      const char *name;
      char number[0X10];

      if ((pwd = getpwuid(uid))) {
        name = pwd->pw_name;
      } else {
        snprintf(number, sizeof(number), "%d", uid);
        name = number;
      }

      logMessage(LOG_NOTICE, "executing as the invoking user: %s", name);
    }

    if (*(user = configuredUser)) {
      struct passwd *pwd;

      if ((pwd = getpwnam(user))) {
        if (canSwitchGroup(pwd->pw_gid)) {
          gid = pwd->pw_gid;
        }
      }
    }

    setProcessOwnership(uid, gid);
    if (!amPrivilegedUser()) *haveHomeDirectory = 1;
  }

  return 0;
}

static const char *
getSocketsDirectory (void) {
  const char *path = BRLAPI_SOCKETPATH;
  if (!ensureDirectory(path, 1)) path = NULL;
  return path;
}

typedef struct {
  const char *whichDirectory;
  const char *(*getPath) (void);
  const char *expectedName;
} StateDirectoryEntry;

static const StateDirectoryEntry stateDirectoryTable[] = {
  { .whichDirectory = "updatable",
    .getPath = getUpdatableDirectory,
    .expectedName = "brltty",
  },

  { .whichDirectory = "writable",
    .getPath = getWritableDirectory,
    .expectedName = "brltty",
  },

  { .whichDirectory = "sockets",
    .getPath = getSocketsDirectory,
    .expectedName = "BrlAPI",
  },
}; static const uint8_t stateDirectoryCount = ARRAY_COUNT(stateDirectoryTable);

static int
canCreateStateDirectory (void) {
#ifdef CAP_DAC_OVERRIDE
  if (needCapability(CAP_DAC_OVERRIDE, 0, "for creating missing state directories")) {
    return 1;
  }
#endif /* CAP_DAC_OVERRIDE */

  return 0;
}

static const char *
getStateDirectoryPath (const StateDirectoryEntry *sde) {
  {
    const char *path = sde->getPath();
    if (path) return path;
  }

  if (!canCreateStateDirectory()) return NULL;
  return sde->getPath();
}

static int
canChangePathOwnership (const char *path) {
#ifdef CAP_CHOWN
  if (needCapability(CAP_CHOWN, 0, "for claiming ownership of the state directories")) {
    return 1;
  }
#endif /* CAP_CHOWN */

  return 0;
}

static int
canChangePathPermissions (const char *path) {
#ifdef CAP_FOWNER
  if (needCapability(CAP_FOWNER, 0, "for adding group permissions to the state directories")) {
    return 1;
  }
#endif /* CAP_FOWNER */

  return 0;
}

typedef struct {
  uid_t owningUser;
  gid_t owningGroup;
} StateDirectoryData;

static int
claimStateDirectory (const PathProcessorParameters *parameters) {
  const StateDirectoryData *sdd = parameters->data;
  const char *path = parameters->path;
  uid_t user = sdd->owningUser;
  gid_t group = sdd->owningGroup;
  struct stat status;

  if (stat(path, &status) != -1) {
    int ownershipClaimed = 0;

    if ((status.st_uid == user) && (status.st_gid == group)) {
      ownershipClaimed = 1;
    } else if (!canChangePathOwnership(path)) {
      logMessage(LOG_WARNING, "can't claim ownership: %s", path);
    } else if (chown(path, user, group) == -1) {
      logSystemError("chown");
    } else {
      logMessage(LOG_INFO, "ownership claimed: %s", path);
      ownershipClaimed = 1;
    }

    if (ownershipClaimed) {
      mode_t oldMode = status.st_mode;
      mode_t newMode = oldMode;

      newMode |= S_IRGRP | S_IWGRP;
      if (S_ISDIR(newMode)) newMode |= S_IXGRP | S_ISGID;

      if (newMode != oldMode) {
        if (!canChangePathPermissions(path)) {
          logMessage(LOG_WARNING, "can't add group permissions: %s", path);
        } else if (chmod(path, newMode) != -1) {
          logMessage(LOG_INFO, "group permissions added: %s", path);
        } else {
          logSystemError("chmod");
        }
      }
    }
  } else {
    logSystemError("stat");
  }

  return 1;
}

static void
claimStateDirectories (void) {
  StateDirectoryData sdd = {
    .owningUser = geteuid(),
    .owningGroup = getegid(),
  };

  const StateDirectoryEntry *sde = stateDirectoryTable;
  const StateDirectoryEntry *end = sde + stateDirectoryCount;

  while (sde < end) {
    const char *path = getStateDirectoryPath(sde);

    if (path && *path) {
      const char *name = locatePathName(path);

      if (strcasecmp(name, sde->expectedName) == 0) {
        processPathTree(path, claimStateDirectory, &sdd);
      } else {
        logMessage(LOG_WARNING,
          "not claiming %s directory: %s (expecting %s)",
          sde->whichDirectory, path, sde->expectedName
        );
      }
    }

    sde += 1;
  }
}
#endif /* HAVE_PWD_H */

typedef enum {
  PARM_PATH,
  PARM_SECCOMP,
  PARM_SHELL,
  PARM_USER,
} Parameters;

const char *const *
getPrivilegeParameterNames (void) {
  static const char *const names[] = NULL_TERMINATED_STRING_ARRAY(
    "path", "seccomp", "shell", "user"
  );

  return names;
}

const char *
getPrivilegeParametersPlatform (void) {
  return "lx";
}

void
establishProgramPrivileges (char **specifiedParameters, char **configuredParameters) {
  logCurrentCapabilities("at start");

  setCommandSearchPath(specifiedParameters[PARM_PATH]);
  setDefaultShell(specifiedParameters[PARM_SHELL]);

#ifdef PR_SET_KEEPCAPS
  if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == -1) {
    logSystemError("prctl[PR_SET_KEEPCAPS]");
  }
#endif /* PR_SET_KEEPCAPS */

#ifdef HAVE_SCHED_H
  isolateNamespaces();
#endif /* HAVE_SCHED_H */

  {
    int haveHomeDirectory = 0;

#ifdef HAVE_PWD_H
    int switched = switchUser(
      specifiedParameters[PARM_USER],
      configuredParameters[PARM_USER],
      &haveHomeDirectory
    );

    if (switched) {
      umask(umask(0) & ~S_IRWXG);
      claimStateDirectories();
    } else {
      logMessage(LOG_DEBUG, "not claiming state directories");
    }

    endpwent();
#endif /* HAVE_PWD_H */

    if (!haveHomeDirectory) {
      if (!setHomeDirectory(getUpdatableDirectory())) {
        logMessage(LOG_WARNING, "home directory not set");
      }
    }
  }

  acquirePrivileges();
  logCurrentCapabilities("after relinquish");

  #ifdef SECCOMP_MODE_FILTER
  installSecureComputingFilter(specifiedParameters[PARM_SECCOMP]);
  #endif /* SECCOMP_MODE_FILTER */
}
