use chrono::Local;
use clap::Parser;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, BufWriter, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use std::thread;
use std::time::Duration;

#[derive(Parser)]
#[command(author, version, about)]
struct Args {
    /// Path to the persistent idkfs image
    #[arg(long, value_name = "PATH")]
    image: PathBuf,
    /// Desired mount point for idkfs
    #[arg(long, value_name = "PATH")]
    mount: PathBuf,
    /// Where the unix socket for snapshot control lives
    #[arg(long, default_value = "/run/idkfsd.sock", value_name = "PATH")]
    socket: PathBuf,
    /// Snapshot store directory (defaults to <image>.snapshots)
    #[arg(long, value_name = "DIR")]
    store: Option<PathBuf>,
    /// Path to the idkfs FUSE binary (defaults to ./idkfs_fuse)
    #[arg(long, default_value = "./idkfs_fuse", value_name = "BIN")]
    fuse_bin: PathBuf,
    /// Extra arguments passed to the FUSE binary (e.g. `--image`)
    #[arg(long, value_name = "ARG")]
    fuse_arg: Vec<String>,
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

impl SnapshotStore {
    fn new(image: PathBuf, store: PathBuf) -> io::Result<Self> {
        fs::create_dir_all(&store)?;
        Ok(Self { image, store })
    }

    fn meta_path(&self) -> PathBuf {
        self.store.join("list.txt")
    }

    fn next_id_path(&self) -> PathBuf {
        self.store.join("next_id")
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

    fn next_id(&self) -> io::Result<u32> {
        let path = self.next_id_path();
        if !path.exists() {
            return Ok(1);
        }
        let content = fs::read_to_string(path)?;
        Ok(content.trim().parse().unwrap_or(1))
    }

    fn bump_next_id(&self, id: u32) -> io::Result<()> {
        fs::write(self.next_id_path(), id.to_string())?;
        Ok(())
    }

    fn create(&self, desc: &str) -> io::Result<SnapshotMeta> {
        let id = self.next_id()?;
        let target = self.snapshot_path(id);
        fs::copy(&self.image, &target)?;
        let timestamp = Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
        let mut list = self.load_meta()?;
        list.push(SnapshotMeta {
            id,
            timestamp,
            desc: desc.to_string(),
        });
        self.save_meta(&list)?;
        self.bump_next_id(id + 1)?;
        Ok(list.pop().unwrap())
    }

    fn list(&self) -> io::Result<Vec<SnapshotMeta>> {
        self.load_meta()
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
        let source = self.snapshot_path(id);
        if !source.exists() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                "snapshot image missing",
            ));
        }
        fs::copy(&source, &self.image)?;
        Ok(())
    }
}

fn handle_connection(store: Arc<Mutex<SnapshotStore>>, stream: UnixStream) -> io::Result<()> {
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

fn run_server(socket: PathBuf, store: Arc<Mutex<SnapshotStore>>, running: Arc<AtomicBool>) {
    if socket.exists() {
        let _ = fs::remove_file(&socket);
    }
    let listener = match UnixListener::bind(&socket) {
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

fn start_fuse(args: &Args) -> io::Result<Child> {
    let mut cmd = Command::new(&args.fuse_bin);
    cmd.arg(&args.mount);
    for extra in &args.fuse_arg {
        cmd.arg(extra);
    }
    cmd.stdin(Stdio::null())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    cmd.spawn()
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

    while running.load(Ordering::SeqCst) {
        match start_fuse(&args) {
            Ok(mut child) => {
                let status = child.wait()?;
                if !running.load(Ordering::SeqCst) {
                    break;
                }
                eprintln!("idkfsd: fuse exited {:?}", status);
                thread::sleep(Duration::from_secs(1));
            }
            Err(e) => {
                eprintln!("idkfsd: failed to spawn fuse: {}", e);
                thread::sleep(Duration::from_secs(5));
            }
        }
    }

    let _ = fs::remove_file(&args.socket);
    let _ = server_handle.join();
    Ok(())
}
