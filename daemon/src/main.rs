use chrono::Local;
use clap::Parser;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use std::thread;
use std::time::Duration;

#[derive(Parser)]
#[command(author, version, about)]
struct Args {

    #[arg(long, value_name = "PATH")]
    image: PathBuf,

    #[arg(long, value_name = "PATH")]
    mount: PathBuf,

    #[arg(long, default_value = "127.0.0.1:12345", value_name = "ADDR")]
    socket: String,

    #[arg(long, value_name = "DIR")]
    store: Option<PathBuf>,

    #[arg(long, default_value = "kernel", value_name = "MODE")]
    mode: String,

    #[arg(long, default_value = "idkfs", value_name = "FS")]
    fs_type: String,

    #[arg(long, default_value = "/sbin/mount", value_name = "BIN")]
    mount_bin: PathBuf,

    #[arg(long, default_value = "/sbin/umount", value_name = "BIN")]
    umount_bin: PathBuf,
}

struct SnapshotMeta {
    id: u32,
    timestamp: String,
    desc: String,
}

struct SnapshotStore {
    image: PathBuf,
    store: PathBuf,
}
const IDKFS_MAGIC: u32 = 0x49444B46;
const IDKFS_SNAP_MAGIC: u32 = 0x534E4B46;
const IDKFS_BLOCK_SIZE: u64 = 4096;
const IDKFS_MAX_SNAPSHOTS: usize = 64;
const SB_SNAP_REGION_OFF: u64 = 64;
const SB_GENERATION_OFF: u64 = 8;
const SB_ROOT_NODE_OFF: u64 = 16;

#[derive(Clone)]
struct NativeSnapshot {
    id: u32,
    generation: u64,
    root_node: u64,
    name: String,
}

impl SnapshotStore {
    fn new(image: PathBuf, store: PathBuf) -> io::Result<Self> {
        fs::create_dir_all(&store)?;
        Ok(Self { image, store })
    }

    fn meta_path(&self) -> PathBuf {
        self.store.join("list.txt")
    }


    fn snapshot_path(&self, id: u32) -> PathBuf {
        self.store.join(format!("{:04}.img", id))
    }

    fn load_meta(&self) -> io::Result<Vec<SnapshotMeta>> {
        let path = self.meta_path();
        if !path.exists() {
            return Ok(Vec::new());
        }
        let file = File::open(path)?;
        let reader = BufReader::new(file);
        let mut list = Vec::new();
        for line in reader.lines() {
            let line = line?;
            let mut parts = line.splitn(3, '|');
            if let (Some(id), Some(timestamp), Some(desc)) =
                (parts.next(), parts.next(), parts.next())
            {
                if let Ok(id) = id.parse() {
                    list.push(SnapshotMeta {
                        id,
                        timestamp: timestamp.to_string(),
                        desc: desc.to_string(),
                    });
                }
            }
        }
        Ok(list)
    }

    fn save_meta(&self, list: &[SnapshotMeta]) -> io::Result<()> {
        let mut file = File::create(self.meta_path())?;
        for entry in list {
            writeln!(file, "{}|{}|{}", entry.id, entry.timestamp, entry.desc)?;
        }
        Ok(())
    }

    fn read_u32_le(buf: &[u8], off: usize) -> u32 {
        u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
    }
    fn read_u64_le(buf: &[u8], off: usize) -> u64 {
        u64::from_le_bytes([
            buf[off],
            buf[off + 1],
            buf[off + 2],
            buf[off + 3],
            buf[off + 4],
            buf[off + 5],
            buf[off + 6],
            buf[off + 7],
        ])
    }
    fn write_u32_le(buf: &mut [u8], off: usize, v: u32) {
        buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn write_u64_le(buf: &mut [u8], off: usize, v: u64) {
        buf[off..off + 8].copy_from_slice(&v.to_le_bytes());
    }

    fn read_super_fields(&self) -> io::Result<(u64, u64, u64)> {
        let mut f = File::open(&self.image)?;
        let mut sb = vec![0u8; IDKFS_BLOCK_SIZE as usize];
        f.read_exact(&mut sb)?;
        let magic = Self::read_u32_le(&sb, 0);
        if magic != IDKFS_MAGIC {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid idkfs superblock"));
        }
        let generation = Self::read_u64_le(&sb, SB_GENERATION_OFF as usize);
        let root_node = Self::read_u64_le(&sb, SB_ROOT_NODE_OFF as usize);
        let snap_region_block = Self::read_u64_le(&sb, SB_SNAP_REGION_OFF as usize);
        Ok((generation, root_node, snap_region_block))
    }

    fn read_native_table(&self) -> io::Result<(u64, Vec<u8>)> {
        let (_, _, snap_region_block) = self.read_super_fields()?;
        let mut f = File::open(&self.image)?;
        let offset = snap_region_block * IDKFS_BLOCK_SIZE;
        f.seek(SeekFrom::Start(offset))?;
        let mut table = vec![0u8; 4096];
        f.read_exact(&mut table)?;
        if Self::read_u32_le(&table, 0) != IDKFS_SNAP_MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid native snapshot table",
            ));
        }
        Ok((offset, table))
    }

    fn write_native_table(&self, offset: u64, table: &[u8]) -> io::Result<()> {
        let mut f = File::options().read(true).write(true).open(&self.image)?;
        f.seek(SeekFrom::Start(offset))?;
        f.write_all(table)?;
        f.sync_data()?;
        Ok(())
    }

    fn native_list(&self) -> io::Result<Vec<NativeSnapshot>> {
        let (_, table) = self.read_native_table()?;
        let count = Self::read_u32_le(&table, 4) as usize;
        let mut out = Vec::new();
        let base = 16usize;
        let entry_size = 88usize;
        for i in 0..count.min(IDKFS_MAX_SNAPSHOTS) {
            let off = base + i * entry_size;
            let id = Self::read_u32_le(&table, off);
            let generation = Self::read_u64_le(&table, off + 8);
            let root_node = Self::read_u64_le(&table, off + 16);
            let name_raw = &table[off + 24..off + 88];
            let nul = name_raw.iter().position(|b| *b == 0).unwrap_or(name_raw.len());
            let name = String::from_utf8_lossy(&name_raw[..nul]).to_string();
            out.push(NativeSnapshot {
                id,
                generation,
                root_node,
                name,
            });
        }
        Ok(out)
    }

    fn native_create(&self, desc: &str) -> io::Result<NativeSnapshot> {
        let (generation, root_node, _) = self.read_super_fields()?;
        let (offset, mut table) = self.read_native_table()?;
        let count = Self::read_u32_le(&table, 4) as usize;
        let next_id = Self::read_u32_le(&table, 8);
        if count >= IDKFS_MAX_SNAPSHOTS {
            return Err(io::Error::other("native snapshot table full"));
        }
        let base = 16usize;
        let entry_size = 88usize;
        let off = base + count * entry_size;
        let name = if desc.trim().is_empty() {
            Local::now().format("snap-%Y%m%d-%H%M%S").to_string()
        } else {
            desc.trim().to_string()
        };
        Self::write_u32_le(&mut table, off, next_id);
        Self::write_u32_le(&mut table, off + 4, 1);
        Self::write_u64_le(&mut table, off + 8, generation);
        Self::write_u64_le(&mut table, off + 16, root_node);
        let mut name_bytes = [0u8; 64];
        let copy = name.as_bytes();
        let n = copy.len().min(63);
        name_bytes[..n].copy_from_slice(&copy[..n]);
        table[off + 24..off + 88].copy_from_slice(&name_bytes);
        Self::write_u32_le(&mut table, 4, (count + 1) as u32);
        Self::write_u32_le(&mut table, 8, next_id + 1);
        self.write_native_table(offset, &table)?;
        Ok(NativeSnapshot {
            id: next_id,
            generation,
            root_node,
            name,
        })
    }

    fn native_rollback(&self, id: u32) -> io::Result<()> {
        let snaps = self.native_list()?;
        let target = snaps
            .iter()
            .find(|s| s.id == id)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "snapshot not found"))?;
        let mut f = File::options().read(true).write(true).open(&self.image)?;
        let mut sb = vec![0u8; IDKFS_BLOCK_SIZE as usize];
        f.read_exact(&mut sb)?;
        Self::write_u64_le(&mut sb, SB_GENERATION_OFF as usize, target.generation);
        Self::write_u64_le(&mut sb, SB_ROOT_NODE_OFF as usize, target.root_node);
        f.seek(SeekFrom::Start(0))?;
        f.write_all(&sb)?;
        f.sync_data()?;
        Ok(())
    }

    fn create(&self, desc: &str) -> io::Result<SnapshotMeta> {
        let native = self.native_create(desc)?;
        let timestamp = Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
        let mut list = self.load_meta()?;
        list.push(SnapshotMeta {
            id: native.id,
            timestamp,
            desc: format!("{} (gen {})", native.name, native.generation),
        });
        self.save_meta(&list)?;
        Ok(list.pop().unwrap())
    }

    fn list(&self) -> io::Result<Vec<SnapshotMeta>> {
        let native = self.native_list()?;
        let mut out = Vec::new();
        for n in native {
            out.push(SnapshotMeta {
                id: n.id,
                timestamp: format!("gen={}", n.generation),
                desc: n.name,
            });
        }
        Ok(out)
    }

    fn delete(&self, id: u32) -> io::Result<()> {
        let mut list = self.load_meta()?;
        list.retain(|entry| entry.id != id);
        self.save_meta(&list)?;
        let path = self.snapshot_path(id);
        let _ = fs::remove_file(path);
        Ok(())
    }

    fn rollback(&self, id: u32) -> io::Result<()> {
        self.native_rollback(id)
    }
}

fn handle_connection(store: Arc<Mutex<SnapshotStore>>, stream: TcpStream) -> io::Result<()> {
    let mut writer = BufWriter::new(&stream);
    let mut reader = BufReader::new(&stream);
    let mut line = String::new();
    reader.read_line(&mut line)?;
    let trimmed = line.trim_end();
    if trimmed.is_empty() {
        writer.write_all(b"ERR empty command\n")?;
        return Ok(());
    }
    let mut parts = trimmed.splitn(2, '|');
    let cmd = parts.next().unwrap();
    let arg = parts.next().unwrap_or("");
    let store = store.lock().unwrap();
    match cmd {
        "create" => {
            let desc = arg.trim();
            match store.create(desc) {
                Ok(meta) => {
                    writer.write_all(b"OK\n")?;
                    writeln!(writer, "{}|{}|{}", meta.id, meta.timestamp, meta.desc)?;
                }
                Err(e) => {
                    writeln!(writer, "ERR {}", e)?;
                }
            }
        }
        "list" => match store.list() {
            Ok(list) => {
                writer.write_all(b"OK\n")?;
                for entry in list {
                    writeln!(writer, "{}|{}|{}", entry.id, entry.timestamp, entry.desc)?;
                }
            }
            Err(e) => {
                writeln!(writer, "ERR {}", e)?;
            }
        },
        "delete" => match arg.parse::<u32>() {
            Ok(id) => {
                if let Err(e) = store.delete(id) {
                    writeln!(writer, "ERR {}", e)?;
                } else {
                    writer.write_all(b"OK\n")?;
                }
            }
            Err(_) => {
                writer.write_all(b"ERR invalid id\n")?;
            }
        },
        "rollback" => match arg.parse::<u32>() {
            Ok(id) => {
                if let Err(e) = store.rollback(id) {
                    writeln!(writer, "ERR {}", e)?;
                } else {
                    writer.write_all(b"OK\n")?;
                }
            }
            Err(_) => {
                writer.write_all(b"ERR invalid id\n")?;
            }
        },
        other => {
            writeln!(writer, "ERR unknown command '{}'", other)?;
        }
    }
    writer.flush()?;
    Ok(())
}

fn run_server(socket: String, store: Arc<Mutex<SnapshotStore>>, running: Arc<AtomicBool>) {
    let listener: TcpListener = match TcpListener::bind(&socket) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("idkfsd: failed to bind socket: {}", e);
            return;
        }
    };
    for stream in listener.incoming() {
        if !running.load(Ordering::SeqCst) {
            break;
        }
        if let Ok(stream) = stream {
            if let Err(e) = handle_connection(store.clone(), stream) {
                eprintln!("idkfsd: snapshot command failed: {}", e);
            }
        }
    }
}

fn mount_kernel(args: &Args) -> io::Result<()> {
    let status = Command::new(&args.mount_bin)
        .arg("-t")
        .arg(&args.fs_type)
        .arg(&args.image)
        .arg(&args.mount)
        .stdin(Stdio::null())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!("mount failed: {}", status)))
    }
}

fn unmount_kernel(args: &Args) {
    let _ = Command::new(&args.umount_bin)
        .arg(&args.mount)
        .stdin(Stdio::null())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .status();
}

fn main() -> io::Result<()> {
    let args = Args::parse();

    fs::create_dir_all(&args.mount)?;
    let store_path = args
        .store
        .clone()
        .unwrap_or_else(|| PathBuf::from(format!("{}.snapshots", args.image.display())));
    let snapshot_store = Arc::new(Mutex::new(SnapshotStore::new(
        args.image.clone(),
        store_path,
    )?));

    let running = Arc::new(AtomicBool::new(true));
    let term_flag = running.clone();
    ctrlc::set_handler(move || {
        term_flag.store(false, Ordering::SeqCst);
    })
    .expect("failed to set ctrl-c handler");

    let socket_path = args.socket.clone();
    let store_for_server = snapshot_store.clone();
    let running_for_server = running.clone();
    let server_handle =
        thread::spawn(move || run_server(socket_path, store_for_server, running_for_server));

    if args.mode == "kernel" {
        if let Err(e) = mount_kernel(&args) {
            eprintln!("idkfsd: kernel mount failed: {}", e);
            running.store(false, Ordering::SeqCst);
        }
        while running.load(Ordering::SeqCst) {
            thread::sleep(Duration::from_millis(250));
        }
        unmount_kernel(&args);
    } else {
        eprintln!("idkfsd: unsupported mode '{}'", args.mode);
        running.store(false, Ordering::SeqCst);
    }

    let _ = server_handle.join();
    Ok(())
}
