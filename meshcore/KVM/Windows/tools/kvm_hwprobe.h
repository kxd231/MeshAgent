// kvm_h264_check --hw: probe, benchmark and dump of the HARDWARE H.264 encoder (Media Foundation), for a machine with a GPU. See kvm_hwprobe_win.cpp.
#ifndef KVM_HWPROBE_H
#define KVM_HWPROBE_H

// argv[1] is "--hw". Returns the exit code of the process (0 ok, 1 a measurement failed, 2 usage / no Media Foundation, 3 no hardware encoder, 4 a stage hung, 5 time budget used up).
int hwprobe_main(int argc, char **argv);

#endif
