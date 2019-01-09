-- as root: mysql -u root -p mysql
CREATE DATABASE mysqlfs;
GRANT SELECT, INSERT, UPDATE, DELETE ON mysqlfs.* TO 'mysqlfs'@'%' IDENTIFIED BY 'password';
GRANT SELECT, INSERT, UPDATE, DELETE ON mysqlfs.* TO 'mysqlfs'@'localhost' IDENTIFIED BY 'password';
FLUSH PRIVILEGES;
-- check that the mysqlfs subdir was created for you in the data directory
-- as root: mysql -u root -p mysqlfs < schema.sql
