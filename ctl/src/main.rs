use clap::{Parser, Subcommand};
use std::io::{self, BufRead, BufReader, BufWriter, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;

#[derive(Parser)]
#[command(author, version, about)]
struct Args {
    /// Unix-domain socket that idkfsd listens on
    #[arg(short, long, default_value = "/run/idkfsd.sock")]
    socket: PathBuf,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Create a snapshot with an optional description
    Create {
        /// Snapshot description
        #[arg(value_name = "TEXT")]
        desc: Option<String>,
    },
    /// List available snapshots
    List,
    /// Delete a snapshot by ID
    Delete {
        /// Snapshot ID (numeric)
        id: u32,
    },
    /// Roll back the main image to a snapshot
    Rollback {
        /// Snapshot ID (numeric)
        id: u32,
    },
}

fn main() -> io::Result<()> {
    let args = Args::parse();
    let command = args.command;
    let stream = UnixStream::connect(&args.socket)?;
    let mut writer = BufWriter::new(&stream);
    let message = match &command {
        Command::Create { desc } => format!("create|{}\n", desc.clone().unwrap_or_default()),
        Command::List => "list|\n".to_string(),
        Command::Delete { id } => format!("delete|{}\n", id),
        Command::Rollback { id } => format!("rollback|{}\n", id),
    };
    writer.write_all(message.as_bytes())?;
    writer.flush()?;

    let mut reader = BufReader::new(&stream);
    let mut line = String::new();
    if reader.read_line(&mut line)? == 0 {
        eprintln!("idkfsctl: no response from daemon");
        return Ok(());
    }
    if line.starts_with("ERR") {
        eprintln!("{}", line.trim_end());
        return Ok(());
    }

    // print any remaining lines from the daemon
    if command.matches_list_response() || command.matches_create_response() {
        for resp in reader.lines() {
            let resp = resp?;
            if resp.is_empty() {
                continue;
            }
            println!("{}", resp);
        }
    }
    Ok(())
}

trait CommandExt {
    fn matches_list_response(&self) -> bool;
    fn matches_create_response(&self) -> bool;
}

impl CommandExt for Command {
    fn matches_list_response(&self) -> bool {
        matches!(self, Command::List)
    }

    fn matches_create_response(&self) -> bool {
        matches!(self, Command::Create { .. })
    }
}
