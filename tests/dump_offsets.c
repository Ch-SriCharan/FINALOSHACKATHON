#include "common.h"
#include <stddef.h>

int main() {
    printf("sizeof(SharedMemory)=%zu\n", sizeof(SharedMemory));
    printf("simulation_active offset=%zu\n", offsetof(SharedMemory, simulation_active));
    printf("global_lock offset=%zu\n", offsetof(SharedMemory, global_lock));
    printf("patients offset=%zu\n", offsetof(SharedMemory, patients));
    printf("sizeof(PatientRecord)=%zu\n", sizeof(PatientRecord));
    printf("patient_id offset=%zu\n", offsetof(PatientRecord, patient_id));
    printf("name offset=%zu\n", offsetof(PatientRecord, name));
    printf("heart_rate offset=%zu\n", offsetof(PatientRecord, heart_rate));
    printf("spo2 offset=%zu\n", offsetof(PatientRecord, spo2));
    printf("status offset=%zu\n", offsetof(PatientRecord, status));
    printf("timestamp offset=%zu\n", offsetof(PatientRecord, timestamp));
    printf("sequence_number offset=%zu\n", offsetof(PatientRecord, sequence_number));
    printf("sensor_pid offset=%zu\n", offsetof(PatientRecord, sensor_pid));
    printf("lock offset=%zu\n", offsetof(PatientRecord, lock));
    printf("total_sensor_updates offset=%zu\n", offsetof(SharedMemory, total_sensor_updates));
    printf("total_alerts_triggered offset=%zu\n", offsetof(SharedMemory, total_alerts_triggered));
    printf("master_pid offset=%zu\n", offsetof(SharedMemory, master_pid));
    printf("sensor_pids offset=%zu\n", offsetof(SharedMemory, sensor_pids));
    printf("alert_detector_pid offset=%zu\n", offsetof(SharedMemory, alert_detector_pid));
    printf("fifo_listener_pid offset=%zu\n", offsetof(SharedMemory, fifo_listener_pid));
    return 0;
}
