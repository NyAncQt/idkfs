use clap::{Parser, Subcommand};
use std::io::{self, BufRead, BufReader, BufWriter, Write};
use std::net::TcpStream;

#[derive(Parser)]
#[command(author, version, about)]
struct Args {

    #[arg(short, long, default_value = "127.0.0.1:12345")]
    socket: String,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {

    Create {

        #[arg(value_name = "TEXT")]
        desc: Option<String>,
    },

    List,

    Delete {

        id: u32,
    },

    Rollback {

        id: u32,
    },
}

fn main() -> io::Result<()> {
    let args = Args::parse();
    let command = args.command;
    let stream = TcpStream::connect(&args.socket)?;
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


    if command.matches_list_response() || command.matches_create_response() {
        for result in reader.lines() {
            let resp = result?;
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
