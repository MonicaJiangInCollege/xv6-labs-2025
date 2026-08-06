// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
struct rwspinlock {
  uint locked;          // Guards the fields below.
  uint readers;         // Number of current readers.
  uint writer;          // Is there a current writer?
  uint waiting_writers; // Readers wait while a writer is pending.
};
#endif
