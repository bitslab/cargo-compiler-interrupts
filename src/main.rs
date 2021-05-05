mod args;
mod ops;

pub(crate) type CIResult<T> = anyhow::Result<T>;

fn main() -> CIResult<()> {
    ops::util::init_logger();

    if let Err(e) = ops::integrate::run() {
        ops::cargo::clean()?;
        return Err(e);
    };

    println!("done");

    Ok(())
}
