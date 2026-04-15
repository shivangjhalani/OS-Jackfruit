import time
import sys

def cpu_bound_task(duration=30):
    print(f"Starting CPU-bound task for {duration}s...")
    end_time = time.time() + duration
    count = 0
    while time.time() < end_time:
        count += 1 * 1  # Continuous calculation
    print("CPU-bound task complete.")

if __name__ == "__main__":
    cpu_bound_task()