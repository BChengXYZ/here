use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::Duration,
};

use tokio::{sync::watch, time::Instant};

use crate::{
    config::arch_config::WordType,
    device::{DeviceTrait, MemError, PlicDevice, plic::PeriphIrqId},
    task_spawner::DeviceTask,
};

pub const WATCHDOG_INTERRUPT_ID: PeriphIrqId = 62;
pub const WATCHDOG_SIZE: WordType = 0x10;

const CONTROL_ENABLE: u32 = 1 << 0;
const CONTROL_FEED: u32 = 1 << 1;
const CONTROL_CLEAR: u32 = 1 << 2;
const DEFAULT_TIMEOUT_MS: u32 = 1000;

struct WatchdogLayout {
    control: u32,
    timeout_ms: u32,
}

impl WatchdogLayout {
    fn new() -> Self {
        Self {
            control: 0,
            timeout_ms: DEFAULT_TIMEOUT_MS,
        }
    }
}

#[derive(Clone, Copy, Debug)]
enum WatchdogCommand {
    Arm(Instant, u64),
    Cancel,
}

pub struct WatchdogDevice {
    layout: WatchdogLayout,
    pending: Arc<AtomicBool>,
    generation: Arc<AtomicU64>,
    sender: watch::Sender<WatchdogCommand>,
}

pub struct WatchdogTask {
    rx: watch::Receiver<WatchdogCommand>,
    pending: Arc<AtomicBool>,
    generation: Arc<AtomicU64>,
}

impl DeviceTask for WatchdogTask {
    async fn run_simple(mut self) {
        let mut command = WatchdogCommand::Cancel;

        loop {
            match command {
                WatchdogCommand::Arm(deadline, generation) => tokio::select! {
                    result = self.rx.changed() => {
                        if result.is_err() {
                            return;
                        }
                        command = *self.rx.borrow();
                    }

                    _ = tokio::time::sleep_until(deadline) => {
                        if self.generation.load(Ordering::Acquire) == generation {
                            self.pending.store(true, Ordering::Release);
                        }
                        command = WatchdogCommand::Cancel;
                    }
                },
                WatchdogCommand::Cancel => {
                    if self.rx.changed().await.is_err() {
                        return;
                    }
                    command = *self.rx.borrow();
                }
            }
        }
    }
}

impl WatchdogDevice {
    pub fn new() -> (Self, WatchdogTask) {
        let (tx, rx) = watch::channel(WatchdogCommand::Cancel);
        let pending = Arc::new(AtomicBool::new(false));
        let generation = Arc::new(AtomicU64::new(0));

        (
            Self {
                layout: WatchdogLayout::new(),
                pending: pending.clone(),
                generation: generation.clone(),
                sender: tx,
            },
            WatchdogTask {
                rx,
                pending,
                generation,
            },
        )
    }

    fn arm(&mut self) {
        self.pending.store(false, Ordering::Release);
        let generation = self.generation.fetch_add(1, Ordering::AcqRel) + 1;
        let timeout = Duration::from_millis(self.layout.timeout_ms.max(1) as u64);
        let _ = self
            .sender
            .send(WatchdogCommand::Arm(Instant::now() + timeout, generation));
        self.layout.control |= CONTROL_ENABLE;
    }

    fn disarm(&mut self) {
        self.generation.fetch_add(1, Ordering::AcqRel);
        self.pending.store(false, Ordering::Release);
        self.layout.control &= !CONTROL_ENABLE;
        let _ = self.sender.send(WatchdogCommand::Cancel);
    }
}

impl DeviceTrait for WatchdogDevice {
    fn read(&mut self, addr: WordType, len: u32) -> Result<u64, MemError> {
        if len != 4 {
            return Err(MemError::LoadFault);
        }

        let data = match addr {
            0x00 => self.layout.control,
            0x04 => self.layout.timeout_ms,
            0x08 => {
                let pending = u32::from(self.pending.load(Ordering::Acquire));
                let enabled = u32::from(self.layout.control & CONTROL_ENABLE != 0);
                pending | (enabled << 1)
            }
            0x0c => self.generation.load(Ordering::Acquire) as u32,
            _ => return Err(MemError::LoadFault),
        };

        Ok(data as u64)
    }

    fn write(&mut self, addr: WordType, len: u32, data: u64) -> Result<(), MemError> {
        if len != 4 {
            return Err(MemError::StoreFault);
        }

        let data = data as u32;

        match addr {
            0x00 => {
                let enabled = data & CONTROL_ENABLE != 0;
                let feed = data & CONTROL_FEED != 0;
                let clear = data & CONTROL_CLEAR != 0;

                if clear {
                    self.pending.store(false, Ordering::Release);
                }

                if enabled {
                    if feed || self.layout.control & CONTROL_ENABLE == 0 {
                        self.arm();
                    } else {
                        self.layout.control |= CONTROL_ENABLE;
                    }
                } else {
                    self.disarm();
                }
            }
            0x04 => {
                self.layout.timeout_ms = data;
                if self.layout.control & CONTROL_ENABLE != 0 {
                    self.arm();
                }
            }
            0x08 | 0x0c => {}
            _ => return Err(MemError::StoreFault),
        }

        Ok(())
    }

    fn sync(&mut self) {}
}

impl PlicDevice for WatchdogDevice {
    fn irq_level(&mut self) -> bool {
        self.pending.load(Ordering::Acquire) && (self.layout.control & CONTROL_ENABLE != 0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::time::timeout;

    async fn wait_for_pending(pending: &AtomicBool) {
        timeout(Duration::from_secs(1), async {
            while !pending.load(Ordering::Acquire) {
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("watchdog interrupt timed out");
    }

    #[tokio::test]
    async fn fires_after_timeout() {
        let (mut device, task) = WatchdogDevice::new();
        let pending = device.pending.clone();
        let handle = tokio::spawn(task.run_simple());

        device.write_u32(4, 1).unwrap();
        device.write_u32(0, CONTROL_ENABLE | CONTROL_FEED).unwrap();
        wait_for_pending(&pending).await;
        assert!(pending.load(Ordering::Acquire));

        drop(device);
        handle.await.unwrap();
    }

    #[tokio::test]
    async fn feed_restarts_the_countdown() {
        let (mut device, task) = WatchdogDevice::new();
        let pending = device.pending.clone();
        let handle = tokio::spawn(task.run_simple());

        device.write_u32(4, 50).unwrap();
        device.write_u32(0, CONTROL_ENABLE | CONTROL_FEED).unwrap();
        tokio::time::sleep(Duration::from_millis(10)).await;
        device.write_u32(0, CONTROL_ENABLE | CONTROL_FEED).unwrap();
        tokio::time::sleep(Duration::from_millis(10)).await;
        assert!(!pending.load(Ordering::Acquire));

        wait_for_pending(&pending).await;
        assert!(pending.load(Ordering::Acquire));

        drop(device);
        handle.await.unwrap();
    }

    #[test]
    fn clear_bit_drops_pending_interrupt() {
        let (mut device, _task) = WatchdogDevice::new();
        device.pending.store(true, Ordering::Release);

        device.write_u32(0, CONTROL_CLEAR).unwrap();

        assert!(!device.pending.load(Ordering::Acquire));
    }
}
