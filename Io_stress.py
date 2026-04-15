import time
import os

def io_bound_task(duration=30):
    print(f"Starting I/O-bound task for {duration}s...")
    end_time = time.time() + duration
    filename = "test_io.txt"
    while time.time() < end_time:
        with open(filename, "w") as f:
            f.write("test " * 100)
        os.remove(filename)
        time.sleep(0.01)  # Brief sleep to simulate I/O wait
    print("I/O-bound task complete.")

if __name__ == "__main__":
    io_bound_task()